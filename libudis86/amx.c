/* udis86 - libudis86/amx.c
 *
 * Read-only Intel AMX tile static analysis. See amx.h for the public
 * contract. Everything here works on caller owned bytes and never
 * executes machine code.
 */
#include "amx.h"
#include "decode.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* allocation arena                                                   */
/* ------------------------------------------------------------------ */

struct arena {
    void **ptrs;
    size_t n;
    size_t cap;
};

static void *
arena_alloc(struct arena *a, size_t size)
{
    void *p;
    if (a->n == a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 32;
        void **np = (void **) realloc(a->ptrs, ncap * sizeof(*np));
        if (!np) return NULL;
        a->ptrs = np;
        a->cap = ncap;
    }
    p = malloc(size ? size : 1);
    if (!p) return NULL;
    a->ptrs[a->n++] = p;
    return p;
}

static void *
arena_calloc(struct arena *a, size_t size)
{
    void *p = arena_alloc(a, size);
    if (p) memset(p, 0, size);
    return p;
}

static void
arena_free(struct arena *a)
{
    size_t i;
    for (i = 0; i < a->n; ++i) free(a->ptrs[i]);
    free(a->ptrs);
    a->ptrs = NULL;
    a->n = a->cap = 0;
}

/* ------------------------------------------------------------------ */
/* sorted uint64 sets                                                 */
/* ------------------------------------------------------------------ */

struct u64set {
    uint64_t *v;
    size_t n;
};

static int
set_add(struct arena *a, struct u64set *s, uint64_t val)
{
    size_t i;
    for (i = 0; i < s->n; ++i) {
        if (s->v[i] == val) return 1;
        if (s->v[i] > val) break;
    }
    uint64_t *nv = (uint64_t *) arena_alloc(a, (s->n + 1) * sizeof(*nv));
    if (!nv) return 0;
    memcpy(nv, s->v, i * sizeof(*nv));
    nv[i] = val;
    memcpy(nv + i + 1, s->v + i, (s->n - i) * sizeof(*nv));
    s->v = nv;
    s->n++;
    return 1;
}

static int
set_union(struct arena *a, struct u64set *dst, const struct u64set *src)
{
    size_t i;
    for (i = 0; i < src->n; ++i) {
        if (!set_add(a, dst, src->v[i])) return 0;
    }
    return 1;
}

static int
set_equal(const struct u64set *a, const struct u64set *b)
{
    if (a->n != b->n) return 0;
    return memcmp(a->v, b->v, a->n * sizeof(*a->v)) == 0;
}

/* ------------------------------------------------------------------ */
/* register classification                                            */
/* ------------------------------------------------------------------ */

int
ud_amx_is_tile_reg(unsigned ud_reg)
{
    return ud_reg >= UD_R_TMM0 && ud_reg <= UD_R_TMM7;
}

int
ud_amx_is_vector_reg(unsigned ud_reg)
{
    return (ud_reg >= UD_R_XMM0 && ud_reg <= UD_R_XMM15) ||
           (ud_reg >= UD_R_YMM0 && ud_reg <= UD_R_YMM15);
}

int
ud_amx_is_gpr64(unsigned ud_reg)
{
    return ud_reg >= UD_R_RAX && ud_reg <= UD_R_R15;
}

unsigned
ud_amx_gpr_slot(unsigned ud_reg)
{
    return (unsigned) (ud_reg - UD_R_RAX);
}

const char *
ud_amx_site_kind_str(enum ud_amx_site_kind kind)
{
    switch (kind) {
    case UD_AMX_SITE_LDTILECFG:  return "ldtilecfg";
    case UD_AMX_SITE_STTILECFG:  return "sttilecfg";
    case UD_AMX_SITE_TILERELEASE:return "tilerelease";
    case UD_AMX_SITE_TILEZERO:   return "tilezero";
    case UD_AMX_SITE_TILELOADD:  return "tileloadd";
    case UD_AMX_SITE_TILELOADDT1:return "tileloaddt1";
    case UD_AMX_SITE_TILESTORED: return "tilestored";
    case UD_AMX_SITE_TDPBSSD:    return "tdpbssd";
    }
    return "unknown";
}

/* ------------------------------------------------------------------ */
/* internal model                                                     */
/* ------------------------------------------------------------------ */

#define N_GPR 16

struct gpr_state {
    unsigned char known[N_GPR];
    uint64_t val[N_GPR];
};

enum poss_kind { POSS_KNOWN, POSS_UNKNOWN, POSS_RELEASED };

struct poss {
    enum poss_kind kind;
    unsigned cfg_id;              /* KNOWN */
    uint64_t note;                /* UNKNOWN origin site */
    struct u64set load_sites;     /* KNOWN provenance */
    struct u64set cfg_addrs;
    struct u64set release_sites;  /* RELEASED provenance */
};

struct lattice {
    struct poss *p;
    size_t n;
};

struct cfg_content {
    int start_row;
    struct ud_amx_tile_shape shape[UD_AMX_TILES];
    unsigned id;
};

struct d_insn {
    struct ud u;
    uint64_t addr;
};

struct d_block {
    const struct ud_amx_block *src;
    struct d_insn *insns;
    size_t ninsns;
    /* converged entry state */
    struct gpr_state gpr;
    struct lattice lat;
    int reached;
};

struct analyzer {
    const struct ud_amx_input *in;
    struct arena scratch;
    struct arena *ra;         /* arena owned by the returned report */
    struct d_block *blocks;
    size_t nblocks;
    struct cfg_content *cfgs;
    size_t ncfgs;
    /* report accumulators */
    struct ud_amx_insn *r_insns; size_t r_ninsns;
    struct ud_amx_site *r_sites; size_t r_nsites;
    uint64_t *r_clears; size_t r_nclears;
    int failed;
};

/* Heap container keeping the report and the arena that backs every
 * pointer reachable from it alive until ud_amx_report_free(). */
struct report_holder {
    struct arena arena;
    struct ud_amx_report report;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int
find_block(struct analyzer *az, uint64_t addr, size_t *idx)
{
    size_t i;
    for (i = 0; i < az->nblocks; ++i) {
        if (az->blocks[i].src && az->blocks[i].src->addr == addr) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static int64_t
mem_disp(const struct ud_operand *op)
{
    switch (op->offset) {
    case 8:  return op->lval.sbyte;
    case 16: return op->lval.sword;
    case 32: return op->lval.sdword;
    case 64: return op->lval.sqword;
    }
    return 0;
}

static int64_t
imm_sign(const struct ud_operand *op)
{
    switch (op->size) {
    case 8:  return op->lval.sbyte;
    case 16: return op->lval.sword;
    case 32: return op->lval.sdword;
    case 64: return (int64_t) op->lval.uqword;
    }
    return 0;
}

static unsigned
mem_scale(const struct ud_operand *op)
{
    /* udis86 stores 0 for the factor-1 case */
    return op->scale ? op->scale : 1u;
}

static void
copy_mem(struct ud_amx_mem *dst, const struct ud_operand *op)
{
    memset(dst, 0, sizeof(*dst));
    dst->present = 1;
    dst->rip_relative = (op->base == UD_R_RIP);
    dst->base_reg = op->base;
    dst->index_reg = op->index;
    dst->scale = mem_scale(op);
    dst->disp = mem_disp(op);
}

/* Ordinary effective address (config loads). Returns 0 when any
 * component is unknown. */
static int
eval_plain_addr(const struct ud_operand *op, uint64_t next_addr,
                const struct gpr_state *g, uint64_t *out)
{
    uint64_t v = 0;
    if (op->base == UD_R_RIP) {
        *out = next_addr + (uint64_t) (int64_t) op->lval.sdword;
        return 1;
    }
    v = (uint64_t) mem_disp(op);
    if (op->base != UD_NONE) {
        unsigned s = ud_amx_gpr_slot(op->base);
        if (!g->known[s]) return 0;
        v += g->val[s];
    }
    if (op->index != UD_NONE) {
        unsigned s = ud_amx_gpr_slot(op->index);
        if (!g->known[s]) return 0;
        v += g->val[s] * mem_scale(op);
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* lattice operations                                                 */
/* ------------------------------------------------------------------ */

static struct poss *
lat_find(struct lattice *lat, enum poss_kind kind, unsigned cfg_id,
         uint64_t note)
{
    size_t i;
    for (i = 0; i < lat->n; ++i) {
        struct poss *p = &lat->p[i];
        if (p->kind != kind) continue;
        if (kind == POSS_KNOWN && p->cfg_id == cfg_id) return p;
        if (kind == POSS_UNKNOWN && p->note == note) return p;
        if (kind == POSS_RELEASED) return p;
    }
    return NULL;
}

static int
lat_add_poss(struct analyzer *az, struct lattice *lat,
             const struct poss *src)
{
    struct poss *existing = lat_find(lat, src->kind, src->cfg_id, src->note);
    if (existing) {
        if (!set_union(&az->scratch, &existing->load_sites, &src->load_sites))
            return 0;
        if (!set_union(&az->scratch, &existing->cfg_addrs, &src->cfg_addrs))
            return 0;
        if (!set_union(&az->scratch, &existing->release_sites,
                       &src->release_sites))
            return 0;
        return 1;
    }
    struct poss *grown = (struct poss *)
        arena_alloc(&az->scratch, (lat->n + 1) * sizeof(*grown));
    if (!grown) return 0;
    memcpy(grown, lat->p, lat->n * sizeof(*grown));
    grown[lat->n] = *src;
    lat->p = grown;
    lat->n++;
    return 1;
}

static int
lat_merge(struct analyzer *az, struct lattice *dst, const struct lattice *src)
{
    size_t i;
    for (i = 0; i < src->n; ++i) {
        if (!lat_add_poss(az, dst, &src->p[i])) return 0;
    }
    return 1;
}

static int
lat_equal(const struct lattice *a, const struct lattice *b)
{
    size_t i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; ++i) {
        const struct poss *x = &a->p[i], *y = &b->p[i];
        if (x->kind != y->kind || x->cfg_id != y->cfg_id ||
            x->note != y->note)
            return 0;
        if (!set_equal(&x->load_sites, &y->load_sites)) return 0;
        if (!set_equal(&x->cfg_addrs, &y->cfg_addrs)) return 0;
        if (!set_equal(&x->release_sites, &y->release_sites)) return 0;
    }
    return 1;
}

static void
lat_reset(struct lattice *lat)
{
    lat->p = NULL;
    lat->n = 0;
}

static int
lat_set_single(struct analyzer *az, struct lattice *lat, struct poss one)
{
    struct poss *p = (struct poss *) arena_alloc(&az->scratch, sizeof(*p));
    if (!p) return 0;
    *p = one;
    lat->p = p;
    lat->n = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* configuration parsing and interning                                */
/* ------------------------------------------------------------------ */

static const uint8_t *
region_read(struct analyzer *az, uint64_t addr, size_t need)
{
    const struct ud_amx_input *in = az->in;
    size_t i;
    for (i = 0; i < in->nregions; ++i) {
        const struct ud_amx_region *r = &in->regions[i];
        if (addr >= r->addr && need <= r->size &&
            addr - r->addr <= r->size - need) {
            return r->data + (addr - r->addr);
        }
    }
    return NULL;
}

static uint16_t
rd16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

static int
cfg_equal(const struct cfg_content *c, int start_row,
          const struct ud_amx_tile_shape *sh)
{
    int i;
    if (c->start_row != start_row) return 0;
    for (i = 0; i < UD_AMX_TILES; ++i) {
        if (c->shape[i].rows != sh[i].rows ||
            c->shape[i].colsb != sh[i].colsb)
            return 0;
    }
    return 1;
}

static unsigned
intern_cfg(struct analyzer *az, int start_row,
           const struct ud_amx_tile_shape *sh)
{
    size_t i;
    for (i = 0; i < az->ncfgs; ++i) {
        if (cfg_equal(&az->cfgs[i], start_row, sh))
            return az->cfgs[i].id;
    }
    struct cfg_content *grown = (struct cfg_content *)
        arena_alloc(&az->scratch, (az->ncfgs + 1) * sizeof(*grown));
    if (!grown) { az->failed = 1; return 0; }
    memcpy(grown, az->cfgs, az->ncfgs * sizeof(*grown));
    grown[az->ncfgs].start_row = start_row;
    memcpy(grown[az->ncfgs].shape, sh, sizeof(*sh) * UD_AMX_TILES);
    grown[az->ncfgs].id = (unsigned) az->ncfgs;
    az->cfgs = grown;
    return (unsigned) az->ncfgs++;
}

/* Returns a KNOWN possibility id, or -1 when the blob cannot be trusted
 * as a legal palette 1 configuration. */
static int
parse_config(struct analyzer *az, uint64_t blob_addr,
             struct ud_amx_tile_shape *sh_out, int *start_row_out)
{
    const uint8_t *p = region_read(az, blob_addr, UD_AMX_CFG_SIZE);
    int i;
    unsigned palette, start_row;
    unsigned long total = 0;
    if (!p) return -1;
    palette = p[0];
    start_row = p[1];
    if (palette == 0) {
        /* released state, no shapes */
        return -2;
    }
    if (palette != 1 || start_row != 0) return -1;
    for (i = 0; i < UD_AMX_TILES; ++i) {
        sh_out[i].rows = rd16(p + 2 + 2 * i);
        sh_out[i].colsb = rd16(p + 18 + 2 * i);
    }
    for (i = 0; i < UD_AMX_TILES; ++i) {
        unsigned rows = sh_out[i].rows;
        unsigned colsb = sh_out[i].colsb;
        if (rows > UD_AMX_MAX_ROWS || colsb > UD_AMX_MAX_COLSB) return -1;
        if (colsb & 3u) return -1;
        if ((rows == 0) != (colsb == 0)) return -1;
        total += (unsigned long) rows * colsb;
    }
    if (total > 8192ul) return -1;
    *start_row_out = (int) start_row;
    return 0;
}

/* ------------------------------------------------------------------ */
/* AMX instruction classification                                     */
/* ------------------------------------------------------------------ */

static int
amx_site_kind(enum ud_mnemonic_code m, enum ud_amx_site_kind *kind)
{
    switch (m) {
    case UD_Ildtilecfg:   *kind = UD_AMX_SITE_LDTILECFG;   return 1;
    case UD_Isttilecfg:   *kind = UD_AMX_SITE_STTILECFG;   return 1;
    case UD_Itilerelease: *kind = UD_AMX_SITE_TILERELEASE; return 1;
    case UD_Itilezero:    *kind = UD_AMX_SITE_TILEZERO;    return 1;
    case UD_Itileloadd:   *kind = UD_AMX_SITE_TILELOADD;   return 1;
    case UD_Itileloaddt1: *kind = UD_AMX_SITE_TILELOADDT1; return 1;
    case UD_Itilestored:  *kind = UD_AMX_SITE_TILESTORED;  return 1;
    case UD_Itdpbssd:     *kind = UD_AMX_SITE_TDPBSSD;     return 1;
    default: return 0;
    }
}

static int
is_data_tile(enum ud_amx_site_kind kind)
{
    return kind == UD_AMX_SITE_TILELOADD ||
           kind == UD_AMX_SITE_TILELOADDT1 ||
           kind == UD_AMX_SITE_TILESTORED;
}

static int
has_jimm(const struct ud *u)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (u->operand[i].type == UD_OP_JIMM) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* GPR constant propagation                                            */
/* ------------------------------------------------------------------ */

static void
gpr_clear_all(struct gpr_state *g)
{
    memset(g->known, 0, sizeof(g->known));
}

static void
gpr_copy(struct gpr_state *dst, const struct gpr_state *src)
{
    memcpy(dst, src, sizeof(*dst));
}

static int
gpr_equal(const struct gpr_state *a, const struct gpr_state *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Conservative handling of a non-AMX instruction. */
static void
apply_gpr_effect(struct analyzer *az, const struct d_insn *di,
                 struct gpr_state *g, struct lattice *lat)
{
    const struct ud *u = &di->u;
    const struct ud_operand *d = &u->operand[0];
    const struct ud_operand *s = &u->operand[1];

    switch (u->mnemonic) {
    case UD_Imov:
        if (d->type == UD_OP_REG && ud_amx_is_gpr64(d->base) &&
            d->size == 64) {
            unsigned slot = ud_amx_gpr_slot(d->base);
            if (s->type == UD_OP_REG && ud_amx_is_gpr64(s->base) &&
                s->size == 64) {
                unsigned src = ud_amx_gpr_slot(s->base);
                g->known[slot] = g->known[src];
                g->val[slot] = g->val[src];
                return;
            }
            if (s->type == UD_OP_IMM) {
                uint64_t immv;
                if (u->operand[1]._oprcode == OP_sI) {
                    immv = (uint64_t) imm_sign(s);
                } else if (s->size == 32) {
                    immv = s->lval.udword; /* zero-extended imm32 */
                } else {
                    immv = s->lval.uqword;
                }
                g->known[slot] = 1;
                g->val[slot] = immv;
                return;
            }
        }
        break;
    case UD_Ilea:
        if (d->type == UD_OP_REG && ud_amx_is_gpr64(d->base) &&
            d->size == 64 && s->type == UD_OP_MEM) {
            uint64_t addr;
            unsigned slot = ud_amx_gpr_slot(d->base);
            uint64_t next = di->addr + u->inp_ctr;
            if (eval_plain_addr(s, next, g, &addr)) {
                g->known[slot] = 1;
                g->val[slot] = addr;
            } else {
                g->known[slot] = 0;
            }
            return;
        }
        break;
    case UD_Iadd:
    case UD_Isub:
        if (d->type == UD_OP_REG && ud_amx_is_gpr64(d->base) &&
            d->size == 64 && s->type == UD_OP_IMM) {
            unsigned slot = ud_amx_gpr_slot(d->base);
            if (g->known[slot]) {
                int64_t imm = imm_sign(s);
                g->val[slot] = (u->mnemonic == UD_Iadd)
                    ? g->val[slot] + (uint64_t) imm
                    : g->val[slot] - (uint64_t) imm;
            }
            return;
        }
        break;
    case UD_Icmp:
    case UD_Itest:
    case UD_Inop:
        return;
    default:
        if (has_jimm(u)) return; /* ordinary direct control transfer */
        break;
    }

    /* anything else: conservatively drop GPR constants and trust in
     * the tile configuration no longer */
    gpr_clear_all(g);
    struct poss one;
    memset(&one, 0, sizeof(one));
    one.kind = POSS_UNKNOWN;
    one.note = di->addr;
    if (!lat_set_single(az, lat, one)) az->failed = 1;
}

/* ------------------------------------------------------------------ */
/* block decoding                                                     */
/* ------------------------------------------------------------------ */

static int
decode_blocks(struct analyzer *az)
{
    size_t bi;
    for (bi = 0; bi < az->nblocks; ++bi) {
        struct d_block *b = &az->blocks[bi];
        const struct ud_amx_block *src = b->src;
        struct d_insn *list = NULL;
        size_t n = 0, cap = 0;
        size_t off = 0;
        struct ud u;
        ud_init(&u);
        ud_set_mode(&u, 64);
        ud_set_pc(&u, src->addr);
        ud_set_input_buffer(&u, src->bytes, src->size);
        while (off < src->size) {
            unsigned len;
            struct d_insn *grown;
            if (u.error || ud_input_end(&u)) return 0;
            len = ud_decode(&u);
            if (len == 0 || u.error) return 0;
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                grown = (struct d_insn *)
                    arena_alloc(&az->scratch, ncap * sizeof(*grown));
                if (!grown) return 0;
                memcpy(grown, list, n * sizeof(*grown));
                list = grown;
                cap = ncap;
            }
            list[n].u = u;
            list[n].addr = src->addr + off;
            n++;
            off += len;
        }
        if (off != src->size) return 0;
        b->insns = list;
        b->ninsns = n;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* configuration transitions                                          */
/* ------------------------------------------------------------------ */

static void
apply_amx_transition(struct analyzer *az, const struct d_insn *di,
                     enum ud_amx_site_kind kind, struct lattice *lat,
                     const struct gpr_state *g)
{
    const struct ud *u = &di->u;
    struct poss one;

    memset(&one, 0, sizeof(one));
    switch (kind) {
    case UD_AMX_SITE_LDTILECFG: {
        const struct ud_operand *m = &u->operand[0];
        uint64_t addr;
        uint64_t next = di->addr + u->inp_ctr;
        struct ud_amx_tile_shape sh[UD_AMX_TILES];
        int start_row = 0;
        if (m->type != UD_OP_MEM ||
            !eval_plain_addr(m, next, g, &addr)) {
            one.kind = POSS_UNKNOWN;
            one.note = di->addr;
            if (!lat_set_single(az, lat, one)) az->failed = 1;
            return;
        }
        int rc = parse_config(az, addr, sh, &start_row);
        if (rc == -2) {
            /* palette 0: released */
            one.kind = POSS_RELEASED;
            if (!set_add(&az->scratch, &one.release_sites, di->addr))
                az->failed = 1;
            if (!lat_set_single(az, lat, one)) az->failed = 1;
            return;
        }
        if (rc != 0) {
            one.kind = POSS_UNKNOWN;
            one.note = di->addr;
            if (!lat_set_single(az, lat, one)) az->failed = 1;
            return;
        }
        one.kind = POSS_KNOWN;
        one.cfg_id = intern_cfg(az, start_row, sh);
        if (!set_add(&az->scratch, &one.load_sites, di->addr) ||
            !set_add(&az->scratch, &one.cfg_addrs, addr))
            az->failed = 1;
        if (!lat_set_single(az, lat, one)) az->failed = 1;
        return;
    }
    case UD_AMX_SITE_TILERELEASE:
        one.kind = POSS_RELEASED;
        if (!set_add(&az->scratch, &one.release_sites, di->addr))
            az->failed = 1;
        if (!lat_set_single(az, lat, one)) az->failed = 1;
        return;
    case UD_AMX_SITE_STTILECFG:
        /* a configuration write is not a new configuration */
        return;
    default:
        /* tile data / zero / dot product do not change the config */
        return;
    }
}

/* ------------------------------------------------------------------ */
/* fixpoint over blocks                                               */
/* ------------------------------------------------------------------ */

static int
run_block_effects(struct analyzer *az, struct d_block *b,
                  struct gpr_state *g, struct lattice *lat)
{
    size_t i;
    for (i = 0; i < b->ninsns; ++i) {
        struct d_insn *di = &b->insns[i];
        enum ud_amx_site_kind kind;
        if (amx_site_kind(di->u.mnemonic, &kind)) {
            apply_amx_transition(az, di, kind, lat, g);
        } else {
            apply_gpr_effect(az, di, g, lat);
        }
        if (az->failed) return 0;
    }
    return 1;
}

static int
fixpoint(struct analyzer *az, size_t entry_idx)
{
    size_t *queue;
    unsigned char *scheduled;
    size_t qhead = 0, qtail = 0, qcount = 0;
    unsigned iter = 0;

    /* A block occupies at most one queue slot: its live entry state is
     * stored on the block itself, so a state change while the block is
     * already queued is picked up when it is dequeued. The ring therefore
     * never has to hold more than nblocks entries. */
    queue = (size_t *) arena_alloc(&az->scratch,
                                   az->nblocks * sizeof(*queue));
    scheduled = (unsigned char *) arena_calloc(&az->scratch, az->nblocks);
    if (!queue || !scheduled) return 0;

    /* entry GPR constants */
    {
        size_t i;
        struct d_block *e = &az->blocks[entry_idx];
        for (i = 0; i < az->in->ngpr_init; ++i) {
            const struct ud_amx_gpr_init *gi = &az->in->gpr_init[i];
            if (!ud_amx_is_gpr64(gi->reg)) return 0;
            unsigned s = ud_amx_gpr_slot(gi->reg);
            e->gpr.known[s] = 1;
            e->gpr.val[s] = gi->value;
        }
        struct poss one;
        memset(&one, 0, sizeof(one));
        one.kind = POSS_RELEASED; /* palette 0 is the released state */
        if (!lat_set_single(az, &e->lat, one)) return 0;
        e->reached = 1;
    }
    queue[qtail] = entry_idx;
    qtail = (qtail + 1) % az->nblocks;
    qcount = 1;
    scheduled[entry_idx] = 1;

    while (qcount > 0) {
        size_t bi = queue[qhead];
        struct d_block *b = &az->blocks[bi];
        struct gpr_state g;
        struct lattice lat;
        size_t si;
        qhead = (qhead + 1) % az->nblocks;
        qcount--;
        scheduled[bi] = 0;
        if (++iter > 100000u) return 0;

        gpr_copy(&g, &b->gpr);
        lat = b->lat;
        if (!run_block_effects(az, b, &g, &lat)) return 0;

        for (si = 0; si < b->src->nsucc; ++si) {
            size_t sbi;
            struct d_block *sb;
            struct gpr_state merged_gpr;
            struct lattice merged_lat;
            if (!find_block(az, b->src->succ[si], &sbi)) return 0;
            sb = &az->blocks[sbi];

            if (!sb->reached) {
                sb->gpr = g;
                sb->lat = lat;
                sb->reached = 1;
                if (!scheduled[sbi]) {
                    scheduled[sbi] = 1;
                    queue[qtail] = sbi;
                    qtail = (qtail + 1) % az->nblocks;
                    qcount++;
                }
                continue;
            }
            /* GPR join: conflicting constants become unknown */
            gpr_copy(&merged_gpr, &sb->gpr);
            {
                unsigned k;
                for (k = 0; k < N_GPR; ++k) {
                    if (!g.known[k]) {
                        merged_gpr.known[k] = 0;
                    } else if (!sb->gpr.known[k]) {
                        /* keep unknown */
                    } else if (sb->gpr.val[k] != g.val[k]) {
                        merged_gpr.known[k] = 0;
                    }
                }
            }
            lat_reset(&merged_lat);
            if (!lat_merge(az, &merged_lat, &sb->lat)) return 0;
            if (!lat_merge(az, &merged_lat, &lat)) return 0;

            if (!gpr_equal(&merged_gpr, &sb->gpr) ||
                !lat_equal(&merged_lat, &sb->lat)) {
                sb->gpr = merged_gpr;
                sb->lat = merged_lat;
                if (!scheduled[sbi]) {
                    scheduled[sbi] = 1;
                    queue[qtail] = sbi;
                    qtail = (qtail + 1) % az->nblocks;
                    qcount++;
                }
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* report construction                                                */
/* ------------------------------------------------------------------ */

static void *
r_alloc(struct analyzer *az, size_t size)
{
    return arena_alloc(az->ra, size);
}

static void *
r_calloc(struct analyzer *az, size_t size)
{
    return arena_calloc(az->ra, size);
}

static enum ud_amx_opr_kind
classify_reg(unsigned base, unsigned *tile)
{
    *tile = 0;
    if (ud_amx_is_tile_reg(base)) {
        *tile = (unsigned) (base - UD_R_TMM0);
        return UD_AMX_OPR_TILE;
    }
    if (ud_amx_is_vector_reg(base)) return UD_AMX_OPR_VECTOR;
    if (ud_amx_is_gpr64(base) ||
        (base >= UD_R_AL && base <= UD_R_R15))
        return UD_AMX_OPR_GPR;
    if (base >= UD_R_ES && base <= UD_R_GS) return UD_AMX_OPR_SEG;
    return UD_AMX_OPR_GPR;
}

static struct ud_amx_opr *
convert_operands(struct analyzer *az, const struct ud *u, unsigned *cnt)
{
    struct ud_amx_opr *out;
    unsigned n = 0, i;
    for (i = 0; i < 4; ++i) {
        if (u->operand[i].type != UD_NONE) n++;
    }
    out = (struct ud_amx_opr *) r_calloc(az, n * sizeof(*out));
    if (!out) { az->failed = 1; return NULL; }
    for (i = 0; i < n; ++i) {
        const struct ud_operand *op = &u->operand[i];
        struct ud_amx_opr *o = &out[i];
        o->size = op->size;
        switch (op->type) {
        case UD_OP_REG:
            o->kind = classify_reg(op->base, &o->tile);
            o->reg = op->base;
            break;
        case UD_OP_MEM:
            o->kind = UD_AMX_OPR_MEM;
            copy_mem(&o->mem, op);
            break;
        case UD_OP_IMM:
            o->kind = UD_AMX_OPR_IMM;
            o->imm = (op->_oprcode == OP_sI) ? imm_sign(op)
                                              : (int64_t) op->lval.uqword;
            break;
        case UD_OP_JIMM:
            o->kind = UD_AMX_OPR_JIMM;
            o->imm = imm_sign(op);
            break;
        case UD_OP_CONST:
            o->kind = UD_AMX_OPR_CONST;
            o->imm = (int64_t) op->lval.udword;
            break;
        default:
            o->kind = UD_AMX_OPR_NONE;
            break;
        }
    }
    *cnt = n;
    return out;
}

static const struct cfg_content *
find_cfg(struct analyzer *az, unsigned id)
{
    size_t i;
    for (i = 0; i < az->ncfgs; ++i) {
        if (az->cfgs[i].id == id) return &az->cfgs[i];
    }
    return NULL;
}

/* Tile register referenced by a site, and the operand slot it sits in. */
static void
tile_refs_for(struct analyzer *az, const struct ud *u,
              enum ud_amx_site_kind kind,
              const struct ud_amx_tile_ref **refs, unsigned *nrefs)
{
    struct ud_amx_tile_ref *r;
    unsigned n = 0, i;
    for (i = 0; i < 4; ++i) {
        if (u->operand[i].type == UD_OP_REG &&
            ud_amx_is_tile_reg(u->operand[i].base))
            n++;
    }
    r = (struct ud_amx_tile_ref *) r_calloc(az, (n ? n : 1) * sizeof(*r));
    if (!r) { az->failed = 1; *refs = NULL; *nrefs = 0; return; }
    n = 0;
    for (i = 0; i < 4; ++i) {
        const struct ud_operand *op = &u->operand[i];
        enum ud_amx_tile_role role;
        if (op->type != UD_OP_REG || !ud_amx_is_tile_reg(op->base))
            continue;
        if (kind == UD_AMX_SITE_TDPBSSD) {
            role = (i == 0) ? UD_AMX_TILE_RDWR : UD_AMX_TILE_READ;
        } else if (kind == UD_AMX_SITE_TILEZERO ||
                   kind == UD_AMX_SITE_TILELOADD ||
                   kind == UD_AMX_SITE_TILELOADDT1) {
            role = UD_AMX_TILE_WRITE;
        } else {
            role = UD_AMX_TILE_READ; /* tilestored */
        }
        r[n].tile = (unsigned) (op->base - UD_R_TMM0);
        r[n].opr_index = i;
        r[n].role = role;
        n++;
    }
    *refs = r;
    *nrefs = n;
}

static struct ud_amx_state *
snapshot_states(struct analyzer *az, const struct lattice *lat,
                const struct ud *u, enum ud_amx_site_kind kind,
                size_t *out_n)
{
    struct ud_amx_state *st;
    size_t i;
    st = (struct ud_amx_state *)
        r_calloc(az, (lat->n ? lat->n : 1) * sizeof(*st));
    if (!st) { az->failed = 1; return NULL; }
    for (i = 0; i < lat->n; ++i) {
        const struct poss *p = &lat->p[i];
        struct ud_amx_state *s = &st[i];
        s->dims_compatible = -1;
        if (p->kind == POSS_KNOWN) {
            s->kind = UD_AMX_ST_KNOWN;
            s->cfg_id = p->cfg_id;
        } else if (p->kind == POSS_UNKNOWN) {
            s->kind = UD_AMX_ST_UNKNOWN;
            s->cfg_id = UD_AMX_NO_CFG;
            s->note = p->note;
        } else {
            s->kind = UD_AMX_ST_RELEASED;
            s->cfg_id = UD_AMX_NO_CFG;
        }
        s->nload_sites = p->load_sites.n;
        if (p->load_sites.n) {
            s->load_sites = (uint64_t *) r_alloc(az,
                p->load_sites.n * sizeof(uint64_t));
            if (!s->load_sites) { az->failed = 1; return NULL; }
            memcpy((void *) s->load_sites, p->load_sites.v,
                   p->load_sites.n * sizeof(uint64_t));
        }
        s->ncfg_addrs = p->cfg_addrs.n;
        if (p->cfg_addrs.n) {
            s->cfg_addrs = (uint64_t *) r_alloc(az,
                p->cfg_addrs.n * sizeof(uint64_t));
            if (!s->cfg_addrs) { az->failed = 1; return NULL; }
            memcpy((void *) s->cfg_addrs, p->cfg_addrs.v,
                   p->cfg_addrs.n * sizeof(uint64_t));
        }
        s->nrelease_sites = p->release_sites.n;
        if (p->release_sites.n) {
            s->release_sites = (uint64_t *) r_alloc(az,
                p->release_sites.n * sizeof(uint64_t));
            if (!s->release_sites) { az->failed = 1; return NULL; }
            memcpy((void *) s->release_sites, p->release_sites.v,
                   p->release_sites.n * sizeof(uint64_t));
        }
        if (kind == UD_AMX_SITE_TDPBSSD && p->kind == POSS_KNOWN) {
            const struct cfg_content *c = find_cfg(az, p->cfg_id);
            unsigned t0 = (unsigned) (u->operand[0].base - UD_R_TMM0);
            unsigned t1 = (unsigned) (u->operand[1].base - UD_R_TMM0);
            unsigned t2 = (unsigned) (u->operand[2].base - UD_R_TMM0);
            const struct ud_amx_tile_shape *a = &c->shape[t0];
            const struct ud_amx_tile_shape *b = &c->shape[t1];
            const struct ud_amx_tile_shape *d = &c->shape[t2];
            if (a->rows && a->colsb && b->rows && b->colsb &&
                d->rows && d->colsb &&
                a->rows == b->rows && a->colsb == d->colsb &&
                b->colsb == d->rows) {
                s->dims_compatible = 1;
            } else {
                s->dims_compatible = 0;
            }
        }
    }
    *out_n = lat->n;
    return st;
}

/* Per-row half-open byte intervals for a tile data site. The SIB index
 * register is the stride source and is never added to row 0. */
static struct ud_amx_row *
build_rows(struct analyzer *az, const struct d_insn *di,
           const struct ud_amx_mem *mem, unsigned tile,
           const struct lattice *lat, const struct gpr_state *g,
           size_t *out_n, unsigned *site_missing)
{
    const struct ud *u = &di->u;
    uint64_t next = di->addr + u->inp_ctr;
    size_t n = 0;
    struct ud_amx_row *rows = NULL;
    size_t pi;

    for (pi = 0; pi < lat->n; ++pi) {
        const struct poss *p = &lat->p[pi];
        unsigned r;
        unsigned rows_n = 0, colsb = 0;
        int base_known = 0, stride_known = 0;
        uint64_t basev = 0, stridev = 0;
        unsigned base_missing = 0, stride_missing = 0;

        if (p->kind == POSS_KNOWN) {
            const struct cfg_content *c = find_cfg(az, p->cfg_id);
            rows_n = c->shape[tile].rows;
            colsb = c->shape[tile].colsb;
            if (rows_n == 0 || colsb == 0) {
                *site_missing |= UD_AMX_MISS_SHAPE;
                continue;
            }
        }

        /* resolve base + displacement (index deliberately excluded) */
        if (mem->rip_relative) {
            base_known = 1;
            basev = next + (uint64_t) mem->disp;
        } else {
            basev = (uint64_t) mem->disp;
            if (mem->base_reg != UD_NONE) {
                unsigned s = ud_amx_gpr_slot(mem->base_reg);
                if (g->known[s]) { basev += g->val[s]; base_known = 1; }
            } else {
                base_known = 1; /* displacement-only address */
            }
        }
        if (!base_known) base_missing = UD_AMX_MISS_BASE;

        /* resolve stride = index * scale */
        if (mem->index_reg != UD_NONE) {
            unsigned s = ud_amx_gpr_slot(mem->index_reg);
            if (g->known[s]) {
                stride_known = 1;
                stridev = g->val[s] * mem->scale;
            }
        }
        if (!stride_known) stride_missing = UD_AMX_MISS_STRIDE;

        if (p->kind != POSS_KNOWN) {
            struct ud_amx_row *grown = (struct ud_amx_row *)
                r_alloc(az, (n + 1) * sizeof(*grown));
            if (!grown) { az->failed = 1; return NULL; }
            if (rows) memcpy(grown, rows, n * sizeof(*grown));
            rows = grown;
            memset(&rows[n], 0, sizeof(rows[n]));
            rows[n].tile = tile;
            rows[n].row = 0;
            rows[n].cfg_id = UD_AMX_NO_CFG;
            rows[n].sym = *mem;
            rows[n].missing = UD_AMX_MISS_CONFIG | base_missing |
                              stride_missing;
            *site_missing |= rows[n].missing;
            n++;
            continue;
        }

        for (r = 0; r < rows_n; ++r) {
            struct ud_amx_row *grown = (struct ud_amx_row *)
                r_alloc(az, (n + 1) * sizeof(*grown));
            unsigned missing = 0;
            if (!grown) { az->failed = 1; return NULL; }
            if (rows) memcpy(grown, rows, n * sizeof(*grown));
            rows = grown;
            memset(&rows[n], 0, sizeof(rows[n]));
            rows[n].tile = tile;
            rows[n].row = r;
            rows[n].cfg_id = p->cfg_id;
            rows[n].sym = *mem;
            if (base_missing) missing |= base_missing;
            if (r > 0 && stride_missing) missing |= stride_missing;
            rows[n].missing = missing;
            rows[n].resolved = missing ? 0 : 1;
            if (!missing) {
                uint64_t lo = basev + (r ? stridev * r : 0);
                rows[n].lo = lo;
                rows[n].hi = lo + colsb;
            }
            *site_missing |= missing;
            n++;
        }
    }
    if (n == 0) {
        rows = (struct ud_amx_row *) r_calloc(az, sizeof(*rows));
        if (!rows) az->failed = 1;
    }
    *out_n = n;
    return rows;
}

static void
record_clear(struct analyzer *az, uint64_t addr)
{
    uint64_t *grown = (uint64_t *)
        r_alloc(az, (az->r_nclears + 1) * sizeof(*grown));
    if (!grown) { az->failed = 1; return; }
    if (az->r_clears) memcpy(grown, az->r_clears,
                             az->r_nclears * sizeof(*grown));
    grown[az->r_nclears++] = addr;
    az->r_clears = grown;
}

static int
build_report(struct analyzer *az, size_t entry_idx)
{
    size_t *order;
    size_t norder = 0, ohead = 0;
    unsigned char *visited;
    size_t bi;

    order = (size_t *) arena_alloc(&az->scratch,
                                   az->nblocks * sizeof(*order));
    visited = (unsigned char *) arena_calloc(&az->scratch, az->nblocks);
    if (!order || !visited) return 0;
    order[norder++] = entry_idx;
    visited[entry_idx] = 1;
    while (ohead < norder) {
        struct d_block *b = &az->blocks[order[ohead++]];
        size_t si;
        for (si = 0; si < b->src->nsucc; ++si) {
            size_t sbi;
            if (!find_block(az, b->src->succ[si], &sbi)) return 0;
            if (!visited[sbi] && az->blocks[sbi].reached) {
                visited[sbi] = 1;
                order[norder++] = sbi;
            }
        }
    }

    for (bi = 0; bi < norder; ++bi) {
        struct d_block *b = &az->blocks[order[bi]];
        struct gpr_state g;
        struct lattice lat;
        size_t ii;
        gpr_copy(&g, &b->gpr);
        lat = b->lat;

        for (ii = 0; ii < b->ninsns; ++ii) {
            struct d_insn *di = &b->insns[ii];
            struct ud *u = &di->u;
            enum ud_amx_site_kind kind;
            int is_amx = amx_site_kind(u->mnemonic, &kind);
            struct ud_amx_insn *ri;
            struct ud_amx_insn *grown = (struct ud_amx_insn *)
                r_alloc(az, (az->r_ninsns + 1) * sizeof(*grown));
            if (!grown) return 0;
            if (az->r_insns) memcpy(grown, az->r_insns,
                                    az->r_ninsns * sizeof(*grown));
            az->r_insns = grown;
            ri = &az->r_insns[az->r_ninsns++];
            memset(ri, 0, sizeof(*ri));
            ri->addr = di->addr;
            ri->block_addr = b->src->addr;
            ri->length = u->inp_ctr;
            ri->mnemonic = (unsigned) u->mnemonic;
            ri->mnemonic_str = ud_lookup_mnemonic(u->mnemonic);
            ri->oprs = convert_operands(az, u, &ri->noprs);
            ri->gpr_val = (uint64_t *) r_alloc(az,
                N_GPR * sizeof(uint64_t));
            ri->gpr_known = (unsigned char *) r_alloc(az,
                N_GPR * sizeof(unsigned char));
            if (!ri->gpr_val || !ri->gpr_known) return 0;
            memcpy((void *) ri->gpr_val, g.val, sizeof(g.val));
            memcpy((void *) ri->gpr_known, g.known, sizeof(g.known));
            ri->site_index = -1;

            if (is_amx) {
                struct ud_amx_site *sg = (struct ud_amx_site *)
                    r_calloc(az, sizeof(*sg));
                struct ud_amx_site *sites_grown = (struct ud_amx_site *)
                    r_alloc(az, (az->r_nsites + 1) * sizeof(*sites_grown));
                if (!sg || !sites_grown) return 0;
                /* Apply the configuration transition first so that a
                 * LDTILECFG/TILERELEASE site reports the state that it
                 * establishes rather than the state it was entered in.
                 * Data, TILEZERO, TDPBSSD and STTILECFG leave the lattice
                 * unchanged, so ordering is immaterial for them. */
                apply_amx_transition(az, di, kind, &lat, &g);
                if (az->failed) return 0;
                if (az->r_sites) memcpy(sites_grown, az->r_sites,
                                        az->r_nsites * sizeof(*sites_grown));
                az->r_sites = sites_grown;
                az->r_sites[az->r_nsites] = *sg;
                {
                    struct ud_amx_site *s = &az->r_sites[az->r_nsites];
                    int mem_op = (kind == UD_AMX_SITE_LDTILECFG ||
                                  kind == UD_AMX_SITE_STTILECFG ||
                                  is_data_tile(kind));
                    s->kind = kind;
                    s->addr = di->addr;
                    s->length = u->inp_ctr;
                    s->config_load = (kind == UD_AMX_SITE_LDTILECFG);
                    s->config_write = (kind == UD_AMX_SITE_STTILECFG);
                    s->released = (kind == UD_AMX_SITE_TILERELEASE);
                    if (mem_op) {
                        /* The memory operand position varies: it is
                         * operand[0] for the config and tilestored forms
                         * but operand[1] for the tile load forms, so
                         * locate it by type rather than fixed index. */
                        const struct ud_operand *m = NULL;
                        int k;
                        for (k = 0; k < 4; ++k) {
                            if (u->operand[k].type == UD_OP_MEM)
                                m = &u->operand[k];
                        }
                        if (m) copy_mem(&s->mem, m);
                    }
                    tile_refs_for(az, u, kind, &s->tiles, &s->ntiles);
                    s->states = snapshot_states(az, &lat, u, kind,
                                                &s->nstates);
                    if (!s->states) return 0;
                    if (is_data_tile(kind)) {
                        unsigned tile = s->ntiles ? s->tiles[0].tile : 0;
                        s->rows = build_rows(az, di, &s->mem, tile,
                                             &lat, &g, &s->nrows,
                                             &s->missing);
                        if (!s->rows) return 0;
                    }
                    ri->site_index = (int) az->r_nsites;
                    az->r_nsites++;
                }
            } else {
                int was_unknown_before = 0;
                size_t li;
                for (li = 0; li < lat.n; ++li) {
                    if (lat.p[li].kind == POSS_UNKNOWN)
                        was_unknown_before = 1;
                }
                apply_gpr_effect(az, di, &g, &lat);
                if (!was_unknown_before) {
                    for (li = 0; li < lat.n; ++li) {
                        if (lat.p[li].kind == POSS_UNKNOWN) {
                            ri->cleared = 1;
                            record_clear(az, di->addr);
                            break;
                        }
                    }
                }
            }
            if (az->failed) return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* public entry point                                                 */
/* ------------------------------------------------------------------ */

const struct ud_amx_report *
ud_amx_analyze(const struct ud_amx_input *in)
{
    struct analyzer az;
    struct report_holder *holder;
    struct ud_amx_report *report;
    size_t i, entry_idx;

    if (!in || !in->blocks || in->nblocks == 0) return NULL;
    memset(&az, 0, sizeof(az));
    az.in = in;
    holder = (struct report_holder *) calloc(1, sizeof(*holder));
    if (!holder) return NULL;
    az.ra = &holder->arena;

    az.blocks = (struct d_block *)
        arena_calloc(&az.scratch, in->nblocks * sizeof(*az.blocks));
    if (!az.blocks) goto fail;
    az.nblocks = in->nblocks;
    for (i = 0; i < in->nblocks; ++i) {
        const struct ud_amx_block *b = &in->blocks[i];
        if (!b->bytes || b->size == 0) goto fail;
        az.blocks[i].src = b;
    }
    /* successor validation only after every block has a non-null src */
    for (i = 0; i < in->nblocks; ++i) {
        const struct ud_amx_block *b = &in->blocks[i];
        size_t k;
        for (k = 0; k < b->nsucc; ++k) {
            size_t dummy;
            if (!find_block(&az, b->succ[k], &dummy)) goto fail;
        }
    }
    for (i = 0; i < in->nregions; ++i) {
        if (in->regions[i].size && !in->regions[i].data) goto fail;
    }
    for (i = 0; i < in->ngpr_init; ++i) {
        if (!ud_amx_is_gpr64(in->gpr_init[i].reg)) goto fail;
    }
    if (!find_block(&az, in->entry, &entry_idx)) goto fail;

    if (!decode_blocks(&az)) goto fail;
    if (!fixpoint(&az, entry_idx)) goto fail;
    if (!build_report(&az, entry_idx)) goto fail;

    report = &holder->report;
    report->insns = az.r_insns;
    report->ninsns = az.r_ninsns;
    report->sites = az.r_sites;
    report->nsites = az.r_nsites;
    report->clear_sites = az.r_clears;
    report->nclear_sites = az.r_nclears;
    if (az.ncfgs) {
        struct ud_amx_config *cfgs = (struct ud_amx_config *)
            arena_calloc(az.ra, az.ncfgs * sizeof(*cfgs));
        if (!cfgs) goto fail;
        for (i = 0; i < az.ncfgs; ++i) {
            cfgs[i].start_row = az.cfgs[i].start_row;
            memcpy(cfgs[i].shape, az.cfgs[i].shape,
                   sizeof(cfgs[i].shape));
        }
        report->configs = cfgs;
        report->nconfigs = az.ncfgs;
    }
    arena_free(&az.scratch);
    return report;

fail:
    arena_free(&az.scratch);
    arena_free(&holder->arena);
    free(holder);
    return NULL;
}

void
ud_amx_report_free(const struct ud_amx_report *report)
{
    struct report_holder *holder;
    if (!report) return;
    /* struct ud_amx_report is the trailing member of the holder, so the
     * holder address is recoverable by subtracting the arena header. */
    holder = (struct report_holder *)
        ((char *) report - offsetof(struct report_holder, report));
    arena_free(&holder->arena);
    free(holder);
}
