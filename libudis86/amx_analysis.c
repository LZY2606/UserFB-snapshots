/* udis86 - libudis86/amx_analysis.c
 *
 * Read-only AMX tile configuration / data-flow / footprint analysis.
 * No machine code is ever executed: every instruction is decoded with
 * the udis86 decoder and interpreted with the constant-propagation rules
 * documented in amx_analysis.h.
 */
#include "udint.h"
#include "extern.h"
#include "decode.h"
#include "amx_analysis.h"

#if !defined(__UD_STANDALONE__)
# include <string.h>
# include <stdlib.h>
#endif

/* ------------------------------------------------------------------ */
/* allocation arena (analysis lifetime only)                          */
/* ------------------------------------------------------------------ */

struct arena_chunk {
    struct arena_chunk *next;
    size_t              used;
    size_t              cap;
    char                data[1];
};

struct arena {
    struct arena_chunk *head;
};

static void *
am_alloc(struct arena *a, size_t n)
{
    struct arena_chunk *c;
    void *p;
    n = (n + 15u) & ~(size_t) 15u;
    c = a->head;
    if (c == NULL || c->used + n > c->cap) {
        size_t cap = n > 4096 ? n : 4096;
        c = (struct arena_chunk *) malloc(sizeof(*c) + cap);
        if (c == NULL) return NULL;
        c->used = 0;
        c->cap  = cap;
        c->next = a->head;
        a->head = c;
    }
    p = c->data + c->used;
    c->used += n;
    return p;
}

static void
arena_free(struct arena *a)
{
    struct arena_chunk *c = a->head;
    while (c) {
        struct arena_chunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = NULL;
}

/* ------------------------------------------------------------------ */
/* internal model                                                     */
/* ------------------------------------------------------------------ */

struct amx_cfg {
    enum ud_amx_config_kind kind;
    uint8_t  rows[UD_AMX_TILE_COUNT];
    uint16_t colsb[UD_AMX_TILE_COUNT];
    uint64_t sources[64];
    size_t   nsources;
};

struct cfg_state {
    int              gpr_known[16];
    uint64_t         gpr_value[16];
    struct amx_cfg **cfgs;
    size_t           ncfg;
};

struct decoded_insn {
    uint64_t              addr;
    unsigned              len;
    enum ud_mnemonic_code mnemonic;
    struct ud_operand     op[4];
    int                   stride_role;
};

struct rblock {
    const struct ud_amx_block *in;
    struct decoded_insn       *insns;
    size_t                     ninsn;
    struct cfg_state           entry;
    int                        has_entry;
    int                        reachable;
};

struct fp_row {
    unsigned row;
    uint64_t lo;
    uint64_t hi;
    int      complete;
};

struct fp_build {
    const struct amx_cfg *cfg;
    unsigned              tile;
    struct fp_row        *rows;
    size_t                nrows;
    int                   symbolic;
    const char           *unknown_what;
};

struct ause {
    uint64_t               insn_addr;
    enum ud_mnemonic_code  mnemonic;
    unsigned               insn_len;
    struct ud_amx_tile_ref tiles[4];
    size_t                 ntiles;
    struct ud_amx_mem_desc mem;
    int                    has_mem;
    struct amx_cfg       **cfgs;
    size_t                 ncfg;
    struct fp_build       *fps;
    size_t                 nfps;
    int                    is_load;
    int                    is_write;
    int                    dims;
};

struct analysis {
    struct arena                arena;
    const struct ud_amx_input  *input;
    struct rblock              *blocks;
    size_t                      nblocks;
    struct ause                *uses;
    size_t                      nuses, cuses;
    uint64_t                   *clobbers;
    size_t                      nclob, cclob;
    int                         collecting;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int
is_gpr64(const struct ud_operand *o)
{
    return o->type == UD_OP_REG &&
           o->base >= UD_R_RAX && o->base <= UD_R_R15 &&
           o->size == 64;
}

static int
is_tile(const struct ud_operand *o)
{
    return o->type == UD_OP_REG &&
           o->base >= UD_R_TMM0 && o->base <= UD_R_TMM7;
}

static int
gpr_idx(enum ud_type r)
{
    return (int)(r - UD_R_RAX);
}

static int64_t
signed_disp(const struct ud_operand *o)
{
    /* offset == 0 means no displacement; lval is a shared union and may
       hold a stale value from an earlier operand */
    if (o->offset == 0) return 0;
    switch (o->offset) {
    case 8:  return o->lval.sbyte;
    case 16: return o->lval.sword;
    case 32: return o->lval.sdword;
    default: return (int64_t) o->lval.uqword;
    }
}

static int64_t
signed_imm(const struct ud_operand *o)
{
    switch (o->size) {
    case 8:  return o->lval.sbyte;
    case 16: return o->lval.sword;
    case 32: return o->lval.sdword;
    default: return (int64_t) o->lval.uqword;
    }
}

static struct rblock *
find_block(struct analysis *an, uint64_t addr)
{
    size_t i;
    for (i = 0; i < an->nblocks; ++i) {
        if (an->blocks[i].in->addr == addr) return &an->blocks[i];
    }
    return NULL;
}

int
ud_amx_is_mnemonic(enum ud_mnemonic_code m)
{
    switch (m) {
    case UD_Ildtilecfg:
    case UD_Isttilecfg:
    case UD_Itilerelease:
    case UD_Itilezero:
    case UD_Itileloadd:
    case UD_Itileloaddt1:
    case UD_Itilestored:
    case UD_Itdpbssd:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* configuration sets                                                 */
/* ------------------------------------------------------------------ */

static int
cfg_content_equal(const struct amx_cfg *a, const struct amx_cfg *b)
{
    if (a->kind != b->kind) return 0;
    if (a->kind != UD_AMX_CFG_PALETTE1) return 1;
    return memcmp(a->rows, b->rows, sizeof(a->rows)) == 0 &&
           memcmp(a->colsb, b->colsb, sizeof(a->colsb)) == 0;
}

static int
cfg_equal(const struct amx_cfg *a, const struct amx_cfg *b)
{
    size_t i;
    if (!cfg_content_equal(a, b) || a->nsources != b->nsources) return 0;
    for (i = 0; i < a->nsources; ++i) {
        if (a->sources[i] != b->sources[i]) return 0;
    }
    return 1;
}

static struct amx_cfg *
cfg_clone(struct arena *ar, const struct amx_cfg *c)
{
    struct amx_cfg *n = (struct amx_cfg *) am_alloc(ar, sizeof(*n));
    if (n) *n = *c;
    return n;
}

static struct amx_cfg *
cfg_merge_pair(struct arena *ar, const struct amx_cfg *a,
               const struct amx_cfg *b)
{
    struct amx_cfg *m = cfg_clone(ar, a);
    size_t i, j;
    if (!m) return NULL;
    for (i = 0; i < b->nsources; ++i) {
        int seen = 0;
        for (j = 0; j < m->nsources; ++j) {
            if (m->sources[j] == b->sources[i]) { seen = 1; break; }
        }
        if (!seen && m->nsources < sizeof(m->sources) / sizeof(m->sources[0])) {
            m->sources[m->nsources++] = b->sources[i];
        }
    }
    return m;
}

/*
 * Join two configuration sets into a fresh arena-owned set. Contents
 * that compare equal merge with unioned sources; unknown and known
 * entries may coexist. Per-field maxima are never taken.
 */
static struct amx_cfg **
cfg_join(struct arena *ar, struct amx_cfg *const *a, size_t na,
        struct amx_cfg *const *b, size_t nb, size_t *out_n)
{
    struct amx_cfg **res;
    int *used;
    size_t n = 0, i, j;
    res = (struct amx_cfg **) am_alloc(ar, sizeof(*res) * (na + nb + 1));
    used = (int *) am_alloc(ar, sizeof(int) * (nb + 1));
    if (!res || !used) return NULL;
    memset(used, 0, sizeof(int) * (nb + 1));
    for (i = 0; i < na; ++i) {
        struct amx_cfg *merged = cfg_clone(ar, a[i]);
        for (j = 0; j < nb; ++j) {
            if (!used[j] && cfg_content_equal(a[i], b[j])) {
                struct amx_cfg *tmp = cfg_merge_pair(ar, merged, b[j]);
                if (tmp) merged = tmp;
                used[j] = 1;
            }
        }
        res[n++] = merged;
    }
    for (j = 0; j < nb; ++j) {
        if (!used[j]) res[n++] = cfg_clone(ar, b[j]);
    }
    *out_n = n;
    return res;
}

static void
cfg_set_replace(struct arena *ar, struct cfg_state *st, struct amx_cfg *c)
{
    struct amx_cfg **one =
        (struct amx_cfg **) am_alloc(ar, sizeof(*one));
    if (one) {
        one[0] = c;
        st->cfgs = one;
        st->ncfg = 1;
    }
}

static void
cfg_set_clear(struct cfg_state *st)
{
    st->cfgs = NULL;
    st->ncfg = 0;
}

static int
cfg_state_equal(const struct cfg_state *a, const struct cfg_state *b)
{
    size_t i;
    if (memcmp(a->gpr_known, b->gpr_known, sizeof(a->gpr_known)) != 0 ||
        memcmp(a->gpr_value, b->gpr_value, sizeof(a->gpr_value)) != 0) {
        return 0;
    }
    if (a->ncfg != b->ncfg) return 0;
    for (i = 0; i < a->ncfg; ++i) {
        if (!cfg_equal(a->cfgs[i], b->cfgs[i])) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* GPR state                                                          */
/* ------------------------------------------------------------------ */

static void
state_init(struct cfg_state *st)
{
    memset(st, 0, sizeof(*st));
}

static struct cfg_state *
state_clone(struct arena *ar, const struct cfg_state *s)
{
    struct cfg_state *n = (struct cfg_state *) am_alloc(ar, sizeof(*n));
    size_t i;
    if (!n) return NULL;
    *n = *s;
    if (s->ncfg) {
        struct amx_cfg **cs = (struct amx_cfg **)
            am_alloc(ar, sizeof(*cs) * s->ncfg);
        if (!cs) return NULL;
        for (i = 0; i < s->ncfg; ++i) cs[i] = cfg_clone(ar, s->cfgs[i]);
        n->cfgs = cs;
    }
    return n;
}

/* Per-register lattice join: disagreeing constants become unknown. */
static void
state_join_into(struct arena *ar, struct cfg_state *dst,
                const struct cfg_state *in)
{
    int i;
    for (i = 0; i < 16; ++i) {
        if (!in->gpr_known[i]) {
            dst->gpr_known[i] = 0;
        } else if (dst->gpr_known[i] &&
                   dst->gpr_value[i] != in->gpr_value[i]) {
            dst->gpr_known[i] = 0;
        }
    }
    if (in->ncfg || dst->ncfg) {
        size_t n;
        struct amx_cfg **joined =
            cfg_join(ar, dst->cfgs, dst->ncfg, in->cfgs, in->ncfg, &n);
        if (joined) {
            dst->cfgs = joined;
            dst->ncfg = n;
        }
    }
}

/* ------------------------------------------------------------------ */
/* memory resolution                                                  */
/* ------------------------------------------------------------------ */

/*
 * Resolve a memory operand. With as_stride set the SIB index is the AMX
 * row-stride register: it is never added to the first-row address and is
 * reported through stride_* instead.
 */
static void
resolve_mem(const struct cfg_state *st, const struct decoded_insn *di,
            const struct ud_operand *m, int as_stride,
            struct ud_amx_mem_desc *d)
{
    int64_t disp = signed_disp(m);
    uint64_t addr = 0;
    int known = 1;
    memset(d, 0, sizeof(*d));
    d->present     = 1;
    d->base_reg    = m->base;
    d->index_reg   = m->index;
    d->scale       = m->scale == UD_NONE ? 0 : m->scale;
    d->disp        = disp;
    d->is_rip      = (m->base == UD_R_RIP);
    d->stride_role = as_stride;

    if (as_stride && m->index != UD_NONE) {
        int ix = gpr_idx(m->index);
        d->stride_known = st->gpr_known[ix];
        if (d->stride_known) d->stride_value = st->gpr_value[ix];
    }

    if (d->is_rip) {
        d->base_known = 1;
        d->base_value = di->addr + di->len + (uint64_t) disp;
        return;
    }

    if (m->base != UD_NONE) {
        int bi = gpr_idx(m->base);
        if (!st->gpr_known[bi]) {
            known = 0;
        } else {
            addr = st->gpr_value[bi];
        }
    }
    if (!as_stride && m->index != UD_NONE) {
        int ii = gpr_idx(m->index);
        unsigned sc = m->scale == UD_NONE ? 1 : m->scale;
        if (!st->gpr_known[ii]) {
            known = 0;
        } else {
            addr += st->gpr_value[ii] * sc;
        }
    }
    d->base_known = known;
    if (known) {
        d->base_value = (uint64_t)((int64_t) addr + disp);
    }
}

static const uint8_t *
read_region(const struct analysis *an, uint64_t addr, size_t need)
{
    size_t i;
    for (i = 0; i < an->input->nregions; ++i) {
        const struct ud_amx_ro_region *r = &an->input->regions[i];
        if (addr >= r->addr && addr - r->addr <= r->size &&
            need <= r->size - (addr - r->addr)) {
            return r->data + (addr - r->addr);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* tile configuration interpretation                                  */
/* ------------------------------------------------------------------ */

static int
valid_palette1(const uint8_t *p)
{
    int i;
    if (p[1] != 0) return 0; /* only start_row == 0 is modelled */
    for (i = 0; i < UD_AMX_TILE_COUNT; ++i) {
        uint16_t colsb = (uint16_t)(p[0x18 + 2 * i] |
                                    (p[0x19 + 2 * i] << 8));
        if (p[0x10 + i] > 16) return 0;
        switch (colsb) {
        case 0: case 16: case 32: case 48: case 64: break;
        default: return 0;
        }
    }
    return 1;
}

static struct amx_cfg *
load_config(struct arena *ar, const struct analysis *an,
            const struct cfg_state *st, const struct decoded_insn *di,
            const struct ud_operand *m)
{
    struct ud_amx_mem_desc d;
    struct amx_cfg *c;
    const uint8_t *p;
    int i;

    c = (struct amx_cfg *) am_alloc(ar, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->sources[c->nsources++] = di->addr;
    resolve_mem(st, di, m, 0, &d);

    if (!d.base_known ||
        (p = read_region(an, d.base_value, UD_AMX_CONFIG_SIZE)) == NULL) {
        c->kind = UD_AMX_CFG_UNKNOWN;
        return c;
    }
    if (p[0] == 0) {
        c->kind = UD_AMX_CFG_RELEASED;
        return c;
    }
    if (p[0] == 1 && valid_palette1(p)) {
        c->kind = UD_AMX_CFG_PALETTE1;
        for (i = 0; i < UD_AMX_TILE_COUNT; ++i) {
            c->rows[i]  = p[0x10 + i];
            c->colsb[i] = (uint16_t)(p[0x18 + 2 * i] |
                                     (p[0x19 + 2 * i] << 8));
        }
        return c;
    }
    c->kind = UD_AMX_CFG_UNKNOWN;
    return c;
}

/* ------------------------------------------------------------------ */
/* footprint                                                          */
/* ------------------------------------------------------------------ */

static struct fp_build *
build_footprints(struct arena *ar, const struct cfg_state *st,
                 const struct decoded_insn *di,
                 const struct ud_operand *m, unsigned tile,
                 size_t *out_n)
{
    struct fp_build *fps;
    size_t k;
    fps = (struct fp_build *)
        am_alloc(ar, sizeof(*fps) * (st->ncfg ? st->ncfg : 1));
    if (!fps) { *out_n = 0; return NULL; }
    *out_n = st->ncfg ? st->ncfg : 1;
    memset(fps, 0, sizeof(*fps) * *out_n);

    for (k = 0; k < *out_n; ++k) {
        struct ud_amx_mem_desc d;
        const struct amx_cfg *cfg = st->ncfg ? st->cfgs[k] : NULL;
        struct fp_build *fp = &fps[k];
        fp->cfg = cfg;
        fp->tile = tile;
        resolve_mem(st, di, m, di->stride_role, &d);

        if (cfg == NULL || cfg->kind != UD_AMX_CFG_PALETTE1) {
            fp->symbolic = 1;
            fp->unknown_what =
                "tile shape (configuration is unknown or released)";
            fp->rows = (struct fp_row *) am_alloc(ar, sizeof(*fp->rows));
            if (fp->rows) {
                fp->nrows = 1;
                fp->rows[0].row = UD_AMX_ROW_UNKNOWN;
            }
            continue;
        }
        if (!d.base_known || !d.stride_known) {
            fp->symbolic = 1;
            if (!d.base_known && !d.stride_known) {
                fp->unknown_what = "base address and stride";
            } else if (!d.base_known) {
                fp->unknown_what = "base address";
            } else {
                fp->unknown_what = "stride";
            }
            fp->rows = (struct fp_row *) am_alloc(ar, sizeof(*fp->rows));
            if (fp->rows) {
                fp->nrows = 1;
                fp->rows[0].row = UD_AMX_ROW_UNKNOWN;
            }
            continue;
        }
        {
            unsigned r, nr = cfg->rows[tile];
            uint16_t width = cfg->colsb[tile];
            fp->rows = (struct fp_row *)
                am_alloc(ar, sizeof(*fp->rows) * (nr ? nr : 1));
            if (!fp->rows) continue;
            for (r = 0; r < nr; ++r) {
                uint64_t start = d.base_value + r * d.stride_value;
                fp->rows[fp->nrows].row = r;
                fp->rows[fp->nrows].lo = start;
                fp->rows[fp->nrows].hi = start + width;
                fp->rows[fp->nrows].complete = 1;
                fp->nrows++;
            }
            if (nr == 0) {
                fp->nrows = 1;
                fp->rows[0].row = 0;
                fp->rows[0].lo = d.base_value;
                fp->rows[0].hi = d.base_value;
                fp->rows[0].complete = 1;
            }
        }
    }
    return fps;
}

/* ------------------------------------------------------------------ */
/* TDPBSSD dimension checks                                           */
/* ------------------------------------------------------------------ */

static int
dims_for_config(const struct amx_cfg *cfg, const unsigned tiles[3])
{
    if (cfg->kind != UD_AMX_CFG_PALETTE1) return -1;
    /* dst is MxN, src1 is MxK, src2 is KxN */
    return cfg->rows[tiles[0]]  == cfg->rows[tiles[1]]  &&
           cfg->colsb[tiles[0]] == cfg->colsb[tiles[2]] &&
           cfg->colsb[tiles[1]] == cfg->rows[tiles[2]];
}

/* ------------------------------------------------------------------ */
/* use recording                                                      */
/* ------------------------------------------------------------------ */

static struct ause *
new_use(struct analysis *an, const struct decoded_insn *di)
{
    struct ause *u;
    if (an->nuses == an->cuses) {
        size_t nc = an->cuses ? an->cuses * 2 : 8;
        struct ause *nu = (struct ause *)
            realloc(an->uses, sizeof(*nu) * nc);
        if (!nu) return NULL;
        an->uses = nu;
        an->cuses = nc;
    }
    u = &an->uses[an->nuses++];
    memset(u, 0, sizeof(*u));
    u->insn_addr = di->addr;
    u->mnemonic  = di->mnemonic;
    u->insn_len  = di->len;
    u->dims      = -1;
    return u;
}

static void
snapshot_configs(struct arena *ar, struct ause *u,
                 const struct cfg_state *st)
{
    size_t i;
    u->ncfg = st->ncfg;
    if (!st->ncfg) { u->cfgs = NULL; return; }
    u->cfgs = (struct amx_cfg **)
        am_alloc(ar, sizeof(*u->cfgs) * st->ncfg);
    if (!u->cfgs) return;
    for (i = 0; i < st->ncfg; ++i) {
        u->cfgs[i] = cfg_clone(ar, st->cfgs[i]);
    }
}

static void
add_tile_ref(struct ause *u, const struct ud_operand *o,
             unsigned op_index, enum ud_amx_tile_role role)
{
    struct ud_amx_tile_ref *t;
    if (u->ntiles >= 4 || !is_tile(o)) return;
    t = &u->tiles[u->ntiles++];
    t->tile = (unsigned)(o->base - UD_R_TMM0);
    t->op_index = op_index;
    t->role = role;
}

static const struct ud_operand *
find_mem(const struct decoded_insn *di)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (di->op[i].type == UD_OP_MEM) return &di->op[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* GPR transfer functions                                             */
/* ------------------------------------------------------------------ */

static int
has_jimm(const struct decoded_insn *di)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (di->op[i].type == UD_OP_JIMM) return 1;
    }
    return 0;
}

static void
record_clobber(struct analysis *an, uint64_t addr)
{
    size_t i;
    if (!an->collecting) return;
    for (i = 0; i < an->nclob; ++i) {
        if (an->clobbers[i] == addr) return;
    }
    if (an->nclob == an->cclob) {
        size_t nc = an->cclob ? an->cclob * 2 : 16;
        uint64_t *nn = (uint64_t *)
            realloc(an->clobbers, sizeof(*nn) * nc);
        if (!nn) return;
        an->clobbers = nn;
        an->cclob = nc;
    }
    an->clobbers[an->nclob++] = addr;
}

static void
clobber(struct analysis *an, struct cfg_state *st, uint64_t addr)
{
    int i;
    for (i = 0; i < 16; ++i) st->gpr_known[i] = 0;
    cfg_set_clear(st);
    record_clobber(an, addr);
}

static void
transfer_gpr(struct analysis *an, struct cfg_state *st,
             const struct decoded_insn *di)
{
    const struct ud_operand *d = &di->op[0];
    const struct ud_operand *s = &di->op[1];
    int dst;

    /* ordinary direct control transfers keep the incoming constants */
    if (has_jimm(di)) return;

    switch (di->mnemonic) {
    case UD_Inop:
    case UD_Icmp:
    case UD_Itest:
        return;

    case UD_Imov:
        if (!is_gpr64(d)) { clobber(an, st, di->addr); return; }
        dst = gpr_idx(d->base);
        if (s->type == UD_OP_IMM) {
            st->gpr_known[dst] = 1;
            st->gpr_value[dst] = (uint64_t) signed_imm(s);
        } else if (is_gpr64(s)) {
            int src = gpr_idx(s->base);
            st->gpr_known[dst] = st->gpr_known[src];
            st->gpr_value[dst] = st->gpr_value[src];
        } else {
            st->gpr_known[dst] = 0;
        }
        return;

    case UD_Ilea: {
        struct ud_amx_mem_desc md;
        if (!is_gpr64(d) || s->type != UD_OP_MEM) {
            clobber(an, st, di->addr); return;
        }
        dst = gpr_idx(d->base);
        resolve_mem(st, di, s, 0, &md);
        st->gpr_known[dst] = md.base_known;
        if (md.base_known) st->gpr_value[dst] = md.base_value;
        return;
    }

    case UD_Iadd:
    case UD_Isub: {
        int64_t imm;
        if (!is_gpr64(d) || s->type != UD_OP_IMM) {
            clobber(an, st, di->addr); return;
        }
        dst = gpr_idx(d->base);
        if (!st->gpr_known[dst]) return;
        imm = signed_imm(s);
        st->gpr_value[dst] =
            di->mnemonic == UD_Iadd
                ? st->gpr_value[dst] + (uint64_t) imm
                : st->gpr_value[dst] - (uint64_t) imm;
        return;
    }

    default:
        /* AMX instructions never write general purpose registers */
        if (ud_amx_is_mnemonic(di->mnemonic)) return;
        clobber(an, st, di->addr);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* AMX instruction processing                                         */
/* ------------------------------------------------------------------ */

/* Apply the configuration-state transition of an AMX instruction. */
static void
amx_state_transition(struct analysis *an, struct cfg_state *st,
                     const struct decoded_insn *di)
{
    const struct ud_operand *mem = find_mem(di);
    switch (di->mnemonic) {
    case UD_Ildtilecfg: {
        struct amx_cfg *c =
            mem ? load_config(&an->arena, an, st, di, mem) : NULL;
        if (c) cfg_set_replace(&an->arena, st, c);
        break;
    }
    case UD_Isttilecfg:
        /* a store never becomes a new configuration */
        break;
    case UD_Itilerelease: {
        struct amx_cfg *c =
            (struct amx_cfg *) am_alloc(&an->arena, sizeof(*c));
        if (c) {
            memset(c, 0, sizeof(*c));
            c->kind = UD_AMX_CFG_RELEASED;
            c->sources[c->nsources++] = di->addr;
            cfg_set_replace(&an->arena, st, c);
        }
        break;
    }
    default:
        break;
    }
}

static void
process_amx(struct analysis *an, struct cfg_state *st,
            const struct decoded_insn *di)
{
    struct ause *u;
    const struct ud_operand *mem;
    int i;

    /* data-flow phase: update configuration state without recording */
    if (!an->collecting) {
        amx_state_transition(an, st, di);
        return;
    }

    u = new_use(an, di);
    if (!u) return;
    mem = find_mem(di);

    switch (di->mnemonic) {
    case UD_Ildtilecfg:
        u->is_load = 1;
        if (mem) {
            resolve_mem(st, di, mem, 0, &u->mem);
            u->has_mem = 1;
        }
        amx_state_transition(an, st, di);
        snapshot_configs(&an->arena, u, st);
        break;

    case UD_Isttilecfg:
        u->is_write = 1;
        if (mem) {
            resolve_mem(st, di, mem, 0, &u->mem);
            u->has_mem = 1;
        }
        amx_state_transition(an, st, di);
        snapshot_configs(&an->arena, u, st);
        break;

    case UD_Itilerelease:
        amx_state_transition(an, st, di);
        snapshot_configs(&an->arena, u, st);
        break;

    case UD_Itilezero:
        for (i = 0; i < 4; ++i) {
            if (is_tile(&di->op[i])) {
                add_tile_ref(u, &di->op[i], i, UD_AMX_TILE_WRITE);
            }
        }
        /* TILEZERO produces no memory access */
        snapshot_configs(&an->arena, u, st);
        break;

    case UD_Itileloadd:
    case UD_Itileloaddt1:
        for (i = 0; i < 4; ++i) {
            if (is_tile(&di->op[i])) {
                add_tile_ref(u, &di->op[i], i, UD_AMX_TILE_WRITE);
            }
        }
        if (mem) {
            resolve_mem(st, di, mem, di->stride_role, &u->mem);
            u->has_mem = 1;
            if (u->ntiles) {
                u->fps = build_footprints(&an->arena, st, di, mem,
                                          u->tiles[0].tile, &u->nfps);
            }
        }
        snapshot_configs(&an->arena, u, st);
        break;

    case UD_Itilestored:
        for (i = 0; i < 4; ++i) {
            if (is_tile(&di->op[i])) {
                add_tile_ref(u, &di->op[i], i, UD_AMX_TILE_READ);
            }
        }
        if (mem) {
            resolve_mem(st, di, mem, di->stride_role, &u->mem);
            u->has_mem = 1;
            if (u->ntiles) {
                u->fps = build_footprints(&an->arena, st, di, mem,
                                          u->tiles[0].tile, &u->nfps);
            }
        }
        snapshot_configs(&an->arena, u, st);
        break;

    case UD_Itdpbssd: {
        static const enum ud_amx_tile_role roles[3] = {
            UD_AMX_TILE_RDWR, UD_AMX_TILE_READ, UD_AMX_TILE_READ
        };
        unsigned tiles[3] = {0, 0, 0};
        size_t k;
        int all_known = 1, all_ok = 1;
        for (i = 0; i < 4 && u->ntiles < 3; ++i) {
            if (is_tile(&di->op[i])) {
                add_tile_ref(u, &di->op[i], i, roles[u->ntiles]);
            }
        }
        for (k = 0; k < u->ntiles && k < 3; ++k) tiles[k] = u->tiles[k].tile;
        if (u->ntiles == 3) {
            for (k = 0; k < st->ncfg; ++k) {
                int v = dims_for_config(st->cfgs[k], tiles);
                if (v < 0) all_known = 0;
                else if (!v) all_ok = 0;
            }
            u->dims = (st->ncfg == 0 || !all_known) ? -1 : (all_ok ? 1 : 0);
        }
        snapshot_configs(&an->arena, u, st);
        break;
    }
    default:
        snapshot_configs(&an->arena, u, st);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* block decode                                                       */
/* ------------------------------------------------------------------ */

static int
decode_block(struct rblock *rb)
{
    ud_t ud;
    size_t cap = 8, n = 0, off = 0;
    struct decoded_insn *arr =
        (struct decoded_insn *) malloc(sizeof(*arr) * cap);
    if (!arr) return -1;

    ud_init(&ud);
    ud_set_mode(&ud, 64);
    ud_set_pc(&ud, rb->in->addr);
    ud_set_input_buffer(&ud, rb->in->bytes, rb->in->size);

    while (off < rb->in->size) {
        unsigned len = ud_disassemble(&ud);
        struct decoded_insn di;
        if (len == 0) { free(arr); return -1; }
        memset(&di, 0, sizeof(di));
        di.addr = ud_insn_off(&ud);
        di.len = len;
        di.mnemonic = ud.mnemonic;
        di.stride_role = ud.amx_sib_stride;
        memcpy(di.op, ud.operand, sizeof(di.op));
        if (n == cap) {
            size_t nc = cap * 2;
            struct decoded_insn *na =
                (struct decoded_insn *) realloc(arr, sizeof(*arr) * nc);
            if (!na) { free(arr); return -1; }
            arr = na; cap = nc;
        }
        arr[n++] = di;
        off += len;
    }
    rb->insns = arr;
    rb->ninsn = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* report emission (report-owned, detached from the arena)            */
/* ------------------------------------------------------------------ */

static struct ud_amx_config *
emit_config(const struct amx_cfg *c)
{
    struct ud_amx_config *o =
        (struct ud_amx_config *) calloc(1, sizeof(*o));
    size_t i;
    if (!o) return NULL;
    o->kind = c->kind;
    memcpy(o->rows, c->rows, sizeof(o->rows));
    memcpy(o->colsb, c->colsb, sizeof(o->colsb));
    o->nsources = c->nsources;
    if (c->nsources) {
        o->sources = (uint64_t *)
            malloc(sizeof(*o->sources) * c->nsources);
        if (!o->sources) { free(o); return NULL; }
        for (i = 0; i < c->nsources; ++i) o->sources[i] = c->sources[i];
    }
    return o;
}

static int
cmp_u64(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *) pa, b = *(const uint64_t *) pb;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int
cmp_rblock_addr(const void *pa, const void *pb)
{
    const struct rblock *const *a = pa;
    const struct rblock *const *b = pb;
    uint64_t x = (*a)->in->addr, y = (*b)->in->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static struct ud_amx_report *
emit_report(struct analysis *an)
{
    struct ud_amx_report *rep;
    size_t i;
    rep = (struct ud_amx_report *) calloc(1, sizeof(*rep));
    if (!rep) return NULL;
    rep->nuses = an->nuses;
    if (an->nuses) {
        rep->uses = (struct ud_amx_use *)
            calloc(an->nuses, sizeof(*rep->uses));
        if (!rep->uses) { free(rep); return NULL; }
    }

    for (i = 0; i < an->nuses; ++i) {
        const struct ause *s = &an->uses[i];
        struct ud_amx_use *d = &rep->uses[i];
        size_t k;
        d->insn_addr = s->insn_addr;
        d->mnemonic  = s->mnemonic;
        d->insn_len  = s->insn_len;
        d->ntiles    = s->ntiles;
        memcpy(d->tiles, s->tiles, sizeof(s->tiles));
        d->mem = s->mem;
        d->is_config_load = s->is_load;
        d->is_config_write = s->is_write;
        d->dims_compatible = s->dims;

        d->nconfigs = s->ncfg;
        if (s->ncfg) {
            d->configs = (struct ud_amx_config **)
                calloc(s->ncfg, sizeof(*d->configs));
            d->cfg_dims = (int *) calloc(s->ncfg, sizeof(*d->cfg_dims));
            if (!d->configs || !d->cfg_dims) return NULL;
            for (k = 0; k < s->ncfg; ++k) {
                d->configs[k] = emit_config(s->cfgs[k]);
                if (!d->configs[k]) return NULL;
                if (s->mnemonic == UD_Itdpbssd && s->ntiles == 3) {
                    unsigned t[3];
                    size_t q;
                    for (q = 0; q < 3; ++q) t[q] = s->tiles[q].tile;
                    d->cfg_dims[k] = dims_for_config(s->cfgs[k], t);
                } else {
                    d->cfg_dims[k] = -1;
                }
            }
        }

        d->nfootprints = s->nfps;
        if (s->nfps) {
            d->footprints = (struct ud_amx_footprint *)
                calloc(s->nfps, sizeof(*d->footprints));
            if (!d->footprints) return NULL;
            for (k = 0; k < s->nfps; ++k) {
                const struct fp_build *sfp = &s->fps[k];
                struct ud_amx_footprint *dfp = &d->footprints[k];
                size_t q;
                dfp->tile = sfp->tile;
                dfp->symbolic = sfp->symbolic;
                dfp->unknown_what = sfp->unknown_what;
                dfp->nrows = sfp->nrows;
                dfp->config = k < d->nconfigs ? d->configs[k] : NULL;
                if (sfp->nrows) {
                    dfp->rows = (struct ud_amx_row_range *)
                        calloc(sfp->nrows, sizeof(*dfp->rows));
                    if (!dfp->rows) return NULL;
                    for (q = 0; q < sfp->nrows; ++q) {
                        dfp->rows[q].row = sfp->rows[q].row;
                        dfp->rows[q].lo = sfp->rows[q].lo;
                        dfp->rows[q].hi = sfp->rows[q].hi;
                        dfp->rows[q].complete = sfp->rows[q].complete;
                    }
                }
            }
        }
    }

    if (an->nclob) {
        rep->nclobber_sites = an->nclob;
        rep->clobber_sites = (uint64_t *)
            malloc(sizeof(*rep->clobber_sites) * an->nclob);
        if (!rep->clobber_sites) { free(rep); return NULL; }
        memcpy(rep->clobber_sites, an->clobbers,
               sizeof(*rep->clobber_sites) * an->nclob);
        qsort(rep->clobber_sites, rep->nclobber_sites,
              sizeof(*rep->clobber_sites), cmp_u64);
    }
    return rep;
}

void
ud_amx_report_free(struct ud_amx_report *rep)
{
    size_t i, k;
    if (!rep) return;
    for (i = 0; i < rep->nuses; ++i) {
        struct ud_amx_use *u = &rep->uses[i];
        for (k = 0; k < u->nconfigs; ++k) {
            if (u->configs[k]) {
                free(u->configs[k]->sources);
                free(u->configs[k]);
            }
        }
        free(u->configs);
        free(u->cfg_dims);
        for (k = 0; k < u->nfootprints; ++k) free(u->footprints[k].rows);
        free(u->footprints);
    }
    free(rep->uses);
    free(rep->clobber_sites);
    free(rep);
}

/* ------------------------------------------------------------------ */
/* main entry                                                         */
/* ------------------------------------------------------------------ */

struct ud_amx_report *
ud_amx_analyze(const struct ud_amx_input *input)
{
    struct analysis an;
    struct rblock *entry_block;
    struct cfg_state seed;
    unsigned *stack;
    size_t nstack = 0, cstack = 0;
    size_t i;
    int rc = -1;
    struct ud_amx_report *rep = NULL;

    if (!input || !input->blocks || input->nblocks == 0) return NULL;
    memset(&an, 0, sizeof(an));

    an.input = input;
    an.nblocks = input->nblocks;
    an.blocks = (struct rblock *) calloc(an.nblocks, sizeof(*an.blocks));
    cstack = an.nblocks * 4 + 4;
    stack = (unsigned *) malloc(sizeof(*stack) * cstack);
    if (!an.blocks || !stack) goto out;

    for (i = 0; i < an.nblocks; ++i) {
        an.blocks[i].in = &input->blocks[i];
        state_init(&an.blocks[i].entry);
        if (decode_block(&an.blocks[i]) != 0) goto out;
    }

    entry_block = find_block(&an, input->entry);
    if (!entry_block) goto out;
    state_init(&seed);
    for (i = 0; i < input->nentry_regs; ++i) {
        const struct ud_amx_entry_reg *e = &input->entry_regs[i];
        int ix;
        if (e->reg < UD_R_RAX || e->reg > UD_R_R15) continue;
        ix = gpr_idx(e->reg);
        seed.gpr_known[ix] = 1;
        seed.gpr_value[ix] = e->value;
    }
    entry_block->entry = seed;
    entry_block->has_entry = 1;
    stack[nstack++] = (unsigned)(entry_block - an.blocks);

    /* phase 1: converge GPR/configuration state without recording uses */
    an.collecting = 0;
    while (nstack) {
        unsigned bi = stack[--nstack];
        struct rblock *rb = &an.blocks[bi];
        struct cfg_state *cur = state_clone(&an.arena, &rb->entry);
        size_t ii;
        if (!cur) goto out;
        rb->reachable = 1;

        for (ii = 0; ii < rb->ninsn; ++ii) {
            struct decoded_insn *di = &rb->insns[ii];
            if (ud_amx_is_mnemonic(di->mnemonic)) {
                process_amx(&an, cur, di);
            }
            transfer_gpr(&an, cur, di);
        }

        for (ii = 0; ii < rb->in->nsuccs; ++ii) {
            struct rblock *succ = find_block(&an, rb->in->succs[ii]);
            unsigned idx;
            if (!succ) continue;
            if (!succ->has_entry) {
                struct cfg_state *sc = state_clone(&an.arena, cur);
                if (!sc) goto out;
                succ->entry = *sc;
                succ->has_entry = 1;
            } else {
                struct cfg_state before = succ->entry;
                struct cfg_state merged = succ->entry;
                state_join_into(&an.arena, &merged, cur);
                succ->entry = merged;
                if (cfg_state_equal(&before, &merged)) continue;
            }
            if (nstack == cstack) {
                size_t nc = cstack * 2;
                unsigned *ns = (unsigned *)
                    realloc(stack, sizeof(*ns) * nc);
                if (!ns) goto out;
                stack = ns; cstack = nc;
            }
            idx = (unsigned)(succ - an.blocks);
            stack[nstack++] = idx;
        }
    }

    /*
     * phase 2: replay each reachable block once from its converged entry
     * state, in program (address) order, collecting the report. The
     * intra-block state evolved here is local and discarded, so loops
     * never produce duplicate or intermediate use points.
     */
    {
        struct rblock **ordered = (struct rblock **)
            am_alloc(&an.arena, sizeof(*ordered) * an.nblocks);
        size_t nord = 0, k;
        if (!ordered) goto out;
        for (i = 0; i < an.nblocks; ++i) {
            if (an.blocks[i].reachable) ordered[nord++] = &an.blocks[i];
        }
        qsort(ordered, nord, sizeof(*ordered), cmp_rblock_addr);
        an.collecting = 1;
        for (k = 0; k < nord; ++k) {
            struct rblock *rb = ordered[k];
            struct cfg_state *cur = state_clone(&an.arena, &rb->entry);
            size_t ii;
            if (!cur) goto out;
            for (ii = 0; ii < rb->ninsn; ++ii) {
                struct decoded_insn *di = &rb->insns[ii];
                if (ud_amx_is_mnemonic(di->mnemonic)) {
                    process_amx(&an, cur, di);
                }
                transfer_gpr(&an, cur, di);
            }
        }
    }

    rep = emit_report(&an);
    if (rep) rc = 0;
out:
    free(stack);
    if (an.blocks) {
        for (i = 0; i < an.nblocks; ++i) free(an.blocks[i].insns);
    }
    free(an.blocks);
    free(an.uses);
    free(an.clobbers);
    arena_free(&an.arena);
    if (rc != 0) {
        if (rep) ud_amx_report_free(rep);
        return NULL;
    }
    return rep;
}
