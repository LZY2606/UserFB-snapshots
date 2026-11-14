#include "rv_lrsc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* opcode classification (canonical, post-decompression ops only)     */
/* ------------------------------------------------------------------ */

static int is_lr(rv_op op)
{
    return op == rv_op_lr_w || op == rv_op_lr_d;
}

static int is_sc(rv_op op)
{
    return op == rv_op_sc_w || op == rv_op_sc_d;
}

static int lrsc_width(rv_op op)
{
    return (op == rv_op_lr_w || op == rv_op_sc_w) ? 4 : 8;
}

static int is_branch(rv_op op)
{
    switch (op) {
    case rv_op_beq: case rv_op_bne:
    case rv_op_blt: case rv_op_bge:
    case rv_op_bltu: case rv_op_bgeu:
        return 1;
    default:
        return 0;
    }
}

/* I-base arithmetic/logic allowed inside a retry region. */
static int is_allowed_arith(rv_op op)
{
    switch (op) {
    case rv_op_lui: case rv_op_auipc:
    case rv_op_addi: case rv_op_slti: case rv_op_sltiu:
    case rv_op_xori: case rv_op_ori: case rv_op_andi:
    case rv_op_slli: case rv_op_srli: case rv_op_srai:
    case rv_op_add: case rv_op_sub:
    case rv_op_sll: case rv_op_slt: case rv_op_sltu:
    case rv_op_xor: case rv_op_srl: case rv_op_sra:
    case rv_op_or: case rv_op_and:
    case rv_op_addiw: case rv_op_slliw: case rv_op_srliw: case rv_op_sraiw:
    case rv_op_addw: case rv_op_subw:
    case rv_op_sllw: case rv_op_srlw: case rv_op_sraw:
        return 1;
    default:
        return 0;
    }
}

static int is_load_store(rv_op op)
{
    switch (op) {
    case rv_op_lb: case rv_op_lh: case rv_op_lw:
    case rv_op_lbu: case rv_op_lhu: case rv_op_lwu:
    case rv_op_ld: case rv_op_ldu: case rv_op_lq:
    case rv_op_sb: case rv_op_sh: case rv_op_sw:
    case rv_op_sd: case rv_op_sq:
        return 1;
    default:
        return 0;
    }
}

static int is_amo(rv_op op)
{
    switch (op) {
    case rv_op_lr_w: case rv_op_sc_w:
    case rv_op_amoswap_w: case rv_op_amoadd_w: case rv_op_amoxor_w:
    case rv_op_amoor_w: case rv_op_amoand_w:
    case rv_op_amomin_w: case rv_op_amomax_w:
    case rv_op_amominu_w: case rv_op_amomaxu_w:
    case rv_op_lr_d: case rv_op_sc_d:
    case rv_op_amoswap_d: case rv_op_amoadd_d: case rv_op_amoxor_d:
    case rv_op_amoor_d: case rv_op_amoand_d:
    case rv_op_amomin_d: case rv_op_amomax_d:
    case rv_op_amominu_d: case rv_op_amomaxu_d:
    case rv_op_lr_q: case rv_op_sc_q:
    case rv_op_amoswap_q: case rv_op_amoadd_q: case rv_op_amoxor_q:
    case rv_op_amoor_q: case rv_op_amoand_q:
    case rv_op_amomin_q: case rv_op_amomax_q:
    case rv_op_amominu_q: case rv_op_amomaxu_q:
        return 1;
    default:
        return 0;
    }
}

static int is_muldiv(rv_op op)
{
    switch (op) {
    case rv_op_mul: case rv_op_mulh: case rv_op_mulhsu: case rv_op_mulhu:
    case rv_op_div: case rv_op_divu: case rv_op_rem: case rv_op_remu:
    case rv_op_mulw: case rv_op_divw: case rv_op_divuw:
    case rv_op_remw: case rv_op_remuw:
        return 1;
    default:
        return 0;
    }
}

static int is_system(rv_op op)
{
    /* rv_op_ecall .. rv_op_csrrci form the SYSTEM block in rv_op order */
    return op >= rv_op_ecall && op <= rv_op_csrrci;
}

/* Instructions that may never appear in a proven region (the chosen LR/SC
 * endpoints are exempt by the caller). */
static int is_region_forbidden(rv_op op)
{
    if (is_load_store(op) || is_amo(op) || is_muldiv(op)) return 1;
    if (op == rv_op_fence || op == rv_op_fence_i) return 1;
    if (is_system(op)) return 1;
    if (op == rv_op_jalr) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* reason lists                                                       */
/* ------------------------------------------------------------------ */

static int reason_add(rv_lrsc_reason **out, size_t *n,
    rv_lrsc_reason_kind kind, uint64_t pc, uint64_t ef, uint64_t et)
{
    size_t i;
    rv_lrsc_reason *p;
    for (i = 0; i < *n; i++) {
        if ((*out)[i].kind == kind && (*out)[i].pc == pc &&
            (*out)[i].edge_from == ef && (*out)[i].edge_to == et)
            return 0;
    }
    p = realloc(*out, (*n + 1) * sizeof(*p));
    if (!p) return -1;
    *out = p;
    p[*n].kind = kind;
    p[*n].pc = pc;
    p[*n].edge_from = ef;
    p[*n].edge_to = et;
    (*n)++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* abstract states                                                    */
/* ------------------------------------------------------------------ */

enum { ADDR_TOP = 0, ADDR_BASE = 1 };

typedef struct {
    uint8_t kind;
    uint8_t base;      /* entry register identity */
    int64_t offset;    /* signed constant displacement */
} addr_val;

typedef struct {
    int      owner;    /* node index of the defining SC, -1 = unknown */
    unsigned ver;      /* generation of that SC's status */
} status_val;

static void addr_init(addr_val *a)
{
    a->kind = ADDR_TOP;
    a->base = 0;
    a->offset = 0;
}

static void addr_join(addr_val *dst, const addr_val *in)
{
    if (in->kind == ADDR_TOP || dst->kind == ADDR_TOP) {
        dst->kind = ADDR_TOP;
    } else if (dst->base != in->base || dst->offset != in->offset) {
        dst->kind = ADDR_TOP; /* different paths, no shared proof */
    }
}

static void status_init(status_val *s)
{
    s->owner = -1;
    s->ver = 0;
}

static void status_join(status_val *dst, const status_val *in)
{
    /* A status may confirm a retry only when every predecessor agrees.
     * An unknown lineage on any single path (or differing SC owners)
     * therefore poisons the merge instead of keeping one path's proof. */
    if (dst->owner == -1 || in->owner == -1) {
        dst->owner = -1;
        dst->ver = 0;
    } else if (dst->owner != in->owner || dst->ver != in->ver) {
        dst->owner = -1;
        dst->ver = 0;
    }
}

/* ------------------------------------------------------------------ */
/* analyzer context                                                   */
/* ------------------------------------------------------------------ */

#define MAX_SUCC 2
#define MAX_REGION 16

typedef struct {
    uint32_t succ[MAX_SUCC];
    uint8_t  nsucc;
    uint8_t  reached;
} node_info;

struct analyzer {
    const rv_code_block *blk;
    size_t n;
    node_info *info;

    rv_lrsc_reason *violations;
    size_t nviolations;
};

static void add_violation(struct analyzer *az, rv_lrsc_reason_kind kind,
    uint64_t pc, uint64_t ef, uint64_t et)
{
    reason_add(&az->violations, &az->nviolations, kind, pc, ef, et);
}

/* ------------------------------------------------------------------ */
/* CFG construction and reachability                                  */
/* ------------------------------------------------------------------ */

static void build_cfg(struct analyzer *az)
{
    size_t i;
    for (i = 0; i < az->n; i++) {
        const rv_code_rec *r = &az->blk->recs[i];
        node_info *ni = &az->info[i];
        uint64_t target;

        if (is_branch(r->dec.op)) {
            if (rv_code_rec_branch_target(az->blk, i, &target) == 0) {
                long ti = rv_code_block_find(az->blk, target);
                if (ti >= 0) ni->succ[ni->nsucc++] = (uint32_t)ti;
            }
            if (i + 1 < az->n)
                ni->succ[ni->nsucc++] = (uint32_t)(i + 1);
        } else if (r->dec.op == rv_op_jal) {
            /* JAL x0 / C.J: pure direct jump. A linked JAL writes an
             * unlinkable return target, so it gets no fall-through. */
            if (rv_code_rec_branch_target(az->blk, i, &target) == 0) {
                long ti = rv_code_block_find(az->blk, target);
                if (ti >= 0) ni->succ[ni->nsucc++] = (uint32_t)ti;
            }
        } else if (r->dec.op != rv_op_jalr && i + 1 < az->n) {
            ni->succ[ni->nsucc++] = (uint32_t)(i + 1);
        }
    }
}

static void reach_dfs(struct analyzer *az, size_t u)
{
    unsigned k;
    if (az->info[u].reached) return;
    az->info[u].reached = 1;
    for (k = 0; k < az->info[u].nsucc; k++)
        reach_dfs(az, az->info[u].succ[k]);
}

/* ------------------------------------------------------------------ */
/* phase A: forward collection DFS from the LR                        */
/* Back-edges are recorded (not expanded); nothing is judged yet.     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  *seen;
    size_t    lr_idx;
    size_t   *sc_nodes;
    uint64_t *back_from;
    uint64_t *back_to;
    uint8_t  *back_to_lr;
    size_t    nsc;
    size_t    nback;
    struct analyzer *az;
} fwd_walk;

static void fwd_dfs(fwd_walk *fw, size_t u, unsigned depth)
{
    const rv_code_rec *ru = &fw->az->blk->recs[u];
    unsigned k;

    if (fw->seen[u]) return;
    fw->seen[u] = 1;
    (void)depth;

    if (u != fw->lr_idx && is_sc(ru->dec.op)) {
        fw->sc_nodes[fw->nsc++] = u;
    }

    for (k = 0; k < fw->az->info[u].nsucc; k++) {
        size_t v = fw->az->info[u].succ[k];
        const rv_code_rec *rv = &fw->az->blk->recs[v];
        if (v == fw->lr_idx || v <= u) {
            fw->back_from[fw->nback] = ru->dec.pc;
            fw->back_to[fw->nback] = rv->dec.pc;
            fw->back_to_lr[fw->nback] = (v == fw->lr_idx);
            fw->nback++;
            continue; /* never expand a back-edge */
        }
        fwd_dfs(fw, v, depth + 1);
    }
}

/* ------------------------------------------------------------------ */
/* phase B: region DFS. The status branch is a barrier: only its      */
/* failure edge stays inside the region; its success edge is the      */
/* certificate exit. Constraint evidence is recorded here.            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  *in_region;
    size_t    lr_idx;
    size_t    sc_idx;
    size_t    branch_idx;
    size_t    fail_node;   /* failure-edge successor of the branch */
    size_t    succ_node;   /* success-edge successor of the branch */
    rv_lrsc_reason *reasons;
    size_t   *nreasons;
    struct analyzer *az;
} region_walk;

static void region_dfs(region_walk *rw, size_t u, unsigned depth)
{
    const rv_code_rec *ru = &rw->az->blk->recs[u];
    unsigned k;

    if (rw->in_region[u]) return;
    rw->in_region[u] = 1;

    if (depth > MAX_REGION) {
        reason_add(&rw->reasons, rw->nreasons,
            RV_LRSC_REASON_REGION_TOO_LONG, ru->dec.pc, 0, 0);
    }
    if (u != rw->lr_idx && u != rw->sc_idx) {
        if (is_lr(ru->dec.op)) {
            reason_add(&rw->reasons, rw->nreasons,
                RV_LRSC_REASON_INTERVENING_LR, ru->dec.pc, 0, 0);
        } else if (is_sc(ru->dec.op)) {
            reason_add(&rw->reasons, rw->nreasons,
                RV_LRSC_REASON_FORBIDDEN_INSN, ru->dec.pc, 0, 0);
        } else if (is_region_forbidden(ru->dec.op)) {
            reason_add(&rw->reasons, rw->nreasons,
                RV_LRSC_REASON_FORBIDDEN_INSN, ru->dec.pc, 0, 0);
        } else if (!is_allowed_arith(ru->dec.op) &&
                   !is_branch(ru->dec.op) &&
                   !(ru->dec.op == rv_op_jal &&
                     ru->dec.rd == rv_ireg_zero)) {
            reason_add(&rw->reasons, rw->nreasons,
                RV_LRSC_REASON_FORBIDDEN_INSN, ru->dec.pc, 0, 0);
        }
    }

    for (k = 0; k < rw->az->info[u].nsucc; k++) {
        size_t v = rw->az->info[u].succ[k];
        const rv_code_rec *rv = &rw->az->blk->recs[v];

        if (u == rw->branch_idx) {
            if (v != rw->fail_node) continue; /* success edge leaves */
        }
        if (v == rw->lr_idx) {
            /* legal retry tail; not expanded */
            continue;
        }
        if (v <= u) {
            reason_add(&rw->reasons, rw->nreasons,
                RV_LRSC_REASON_BACKWARD_EDGE, ru->dec.pc,
                ru->dec.pc, rv->dec.pc);
            continue;
        }
        region_dfs(rw, v, depth + 1);
    }
}

/* ------------------------------------------------------------------ */
/* lossless copy recognition and transfer functions                   */
/* ------------------------------------------------------------------ */

static int copy_source(const rv_decode *d, unsigned *src)
{
    if (d->op == rv_op_addi && d->imm == 0) {
        *src = d->rs1;          /* ADDI rd, rs, 0 (includes C.MV) */
        return 1;
    }
    if (d->op == rv_op_add) {   /* ADD with x0 (includes C.ADD forms) */
        if (d->rs2 == rv_ireg_zero) { *src = d->rs1; return 1; }
        if (d->rs1 == rv_ireg_zero) { *src = d->rs2; return 1; }
    }
    return 0;
}

static void transfer_addr(const rv_decode *d, addr_val in[32], addr_val out[32])
{
    unsigned src;
    memcpy(out, in, sizeof(addr_val) * 32);
    if (d->rd == rv_ireg_zero) return;

    if (d->op == rv_op_addi) {
        if (in[d->rs1].kind == ADDR_BASE) {
            out[d->rd].kind = ADDR_BASE;
            out[d->rd].base = in[d->rs1].base;
            out[d->rd].offset = in[d->rs1].offset + (int64_t)d->imm;
        } else {
            addr_init(&out[d->rd]);
        }
    } else if (copy_source(d, &src)) {
        out[d->rd] = in[src];
    } else {
        addr_init(&out[d->rd]); /* any other definition breaks lineage */
    }
}

static void transfer_status(const rv_decode *d, int sc_owner, size_t node,
    status_val in[32], status_val out[32], rv_lrsc_reason **reasons,
    size_t *nreasons)
{
    unsigned src;
    memcpy(out, in, sizeof(status_val) * 32);
    if (d->rd == rv_ireg_zero) return;

    if (is_sc(d->op)) {
        /* SC reads its address register first, then defines the result */
        out[d->rd].owner = sc_owner;
        out[d->rd].ver = 1;
    } else if (copy_source(d, &src)) {
        out[d->rd] = in[src];
    } else {
        /* only an overwrite after the SC can destroy its result; before
         * the SC no register holds its status yet (sc_owner == -1 would
         * otherwise collide with the unknown-owner sentinel). */
        if (sc_owner >= 0 && node > (size_t)sc_owner &&
            in[d->rd].owner == sc_owner) {
            reason_add(reasons, nreasons,
                RV_LRSC_REASON_STATUS_CLOBBERED, d->pc, 0, 0);
        }
        status_init(&out[d->rd]);
    }
}

/* ------------------------------------------------------------------ */
/* tail path: direct JAL x0 / C.J chains from a branch edge           */
/* ------------------------------------------------------------------ */

static int trace_tail(struct analyzer *az, size_t start, int want_lr,
    size_t lr_idx, const uint8_t *region, size_t region_max,
    uint64_t *edge_from, uint64_t *edge_to)
{
    uint8_t *seen = calloc(az->n, 1);
    size_t cur = start;
    int ok = 0;

    if (!seen) return -1;
    while (cur < az->n && !seen[cur]) {
        const rv_code_rec *r = &az->blk->recs[cur];
        seen[cur] = 1;

        if (want_lr) {
            if (cur == lr_idx) { ok = 1; break; }
        } else {
            /* a leaving edge must not re-enter the region or reach the LR */
            if (cur == lr_idx || (cur <= region_max && region[cur]))
                break;
            if (!(r->dec.op == rv_op_jal &&
                  r->dec.rd == rv_ireg_zero &&
                  az->info[cur].nsucc == 1)) {
                ok = 1; break; /* first real instruction outside */
            }
        }

        /* only direct JAL x0 / C.J chains may relay either edge */
        if (r->dec.op == rv_op_jal && r->dec.rd == rv_ireg_zero &&
            az->info[cur].nsucc == 1) {
            *edge_from = r->dec.pc;
            *edge_to = az->blk->recs[az->info[cur].succ[0]].dec.pc;
            cur = az->info[cur].succ[0];
            continue;
        }
        break;
    }
    free(seen);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* per-LR candidate analysis                                          */
/* ------------------------------------------------------------------ */

static int has_pred_outside_region(struct analyzer *az,
    const uint8_t *region, size_t v, const uint64_t *entries,
    size_t entry_count)
{
    size_t i, k;
    for (i = 0; i < az->n; i++) {
        if (region[i] || !az->info[i].reached) continue;
        for (k = 0; k < az->info[i].nsucc; k++) {
            if (az->info[i].succ[k] == v) return 1;
        }
    }
    for (i = 0; i < entry_count; i++) {
        long ei = rv_code_block_find(az->blk, entries[i]);
        if (ei >= 0 && (size_t)ei == v) return 1;
    }
    return 0;
}

static int analyze_lr(struct analyzer *az, size_t lr_idx,
    const uint64_t *entries, size_t entry_count, rv_lrsc_candidate *c)
{
    const rv_code_block *blk = az->blk;
    fwd_walk fw;
    uint8_t *fwd_seen = NULL, *in_region = NULL;
    addr_val (*addr_st)[32] = NULL;
    status_val (*stat_st)[32] = NULL;
    size_t i, region_count = 0, region_max = lr_idx;
    size_t sc_idx = (size_t)-1, branch_idx = (size_t)-1;
    size_t fail_node = (size_t)-1, succ_node = (size_t)-1;
    size_t fwd_max = lr_idx;
    int hard_violation = 0;

    memset(c, 0, sizeof(*c));
    c->verdict = RV_LRSC_VERDICT_UNKNOWN;

    /* ---- phase A: collect everything reachable on forward edges ---- */
    memset(&fw, 0, sizeof(fw));
    fwd_seen = calloc(az->n, 1);
    fw.sc_nodes = malloc(az->n * sizeof(*fw.sc_nodes));
    fw.back_from = malloc(az->n * MAX_SUCC * sizeof(*fw.back_from));
    fw.back_to = malloc(az->n * MAX_SUCC * sizeof(*fw.back_to));
    fw.back_to_lr = malloc(az->n * MAX_SUCC * sizeof(*fw.back_to_lr));
    if (!fwd_seen || !fw.sc_nodes || !fw.back_from || !fw.back_to ||
        !fw.back_to_lr) {
        free(fwd_seen); free(fw.sc_nodes); free(fw.back_from);
        free(fw.back_to); free(fw.back_to_lr);
        return -1;
    }
    fw.seen = fwd_seen;
    fw.lr_idx = lr_idx;
    fw.az = az;
    fwd_dfs(&fw, lr_idx, 1);
    for (i = lr_idx; i < az->n; i++)
        if (fwd_seen[i]) fwd_max = i;
    if (fw.nsc > 0) sc_idx = fw.sc_nodes[0];

    /* ---- dataflow over the forward set (index order is a DAG) ---- */
    addr_st = calloc(az->n, sizeof(*addr_st));
    stat_st = calloc(az->n, sizeof(*stat_st));
    if (!addr_st || !stat_st) goto fail;

    for (i = 0; i < 32; i++) {
        /* every entry register keeps its own identity as a symbolic base */
        addr_st[lr_idx][i].kind = ADDR_BASE;
        addr_st[lr_idx][i].base = (uint8_t)i;
        addr_st[lr_idx][i].offset = 0;
        status_init(&stat_st[lr_idx][i]);
    }

    for (i = lr_idx; i <= fwd_max; i++) {
        const rv_decode *d;
        unsigned k, q;
        size_t p;
        addr_val am[32];
        status_val sm[32];
        int first;

        if (!fwd_seen[i]) continue;
        d = &blk->recs[i].dec;

        if (i != lr_idx) {
            first = 1;
            for (p = lr_idx; p < i; p++) {
                if (!fwd_seen[p]) continue;
                for (k = 0; k < az->info[p].nsucc; k++) {
                    if (az->info[p].succ[k] != i) continue;
                    if (first) {
                        memcpy(am, addr_st[p], sizeof(am));
                        memcpy(sm, stat_st[p], sizeof(sm));
                        first = 0;
                    } else {
                        for (q = 0; q < 32; q++) {
                            addr_join(&am[q], &addr_st[p][q]);
                            status_join(&sm[q], &stat_st[p][q]);
                        }
                    }
                }
            }
            if (first) {
                for (q = 0; q < 32; q++) {
                    addr_init(&am[q]);
                    status_init(&sm[q]);
                }
            }
            memcpy(addr_st[i], am, sizeof(am));
            memcpy(stat_st[i], sm, sizeof(sm));
        }

        if (is_sc(d->op)) {
            if (i == sc_idx) {
                /* address is read before the result is defined */
                const addr_val *sa = &addr_st[i][d->rs1];
                const addr_val *la =
                    &addr_st[lr_idx][blk->recs[lr_idx].dec.rs1];
                if (sa->kind == ADDR_TOP) {
                    reason_add(&c->reasons, &c->reason_count,
                        RV_LRSC_REASON_ADDR_UNKNOWN, d->pc, 0, 0);
                } else if (sa->base != la->base ||
                           sa->offset != la->offset) {
                    reason_add(&c->reasons, &c->reason_count,
                        RV_LRSC_REASON_ADDR_MISMATCH, d->pc, 0, 0);
                }
            }
            transfer_status(d, (int)i, i, stat_st[i], stat_st[i],
                &c->reasons, &c->reason_count);
            addr_init(&addr_st[i][d->rd]); /* result is not an address */
            continue;
        }

        transfer_addr(d, addr_st[i], addr_st[i]);
        transfer_status(d, (int)sc_idx, i, stat_st[i], stat_st[i],
            &c->reasons, &c->reason_count);
    }

    /* ---- locate the branch consuming this SC's status ---- */
    if (sc_idx != (size_t)-1) {
        for (i = sc_idx + 1; i <= fwd_max; i++) {
            const rv_decode *d = &blk->recs[i].dec;
            int reg = -1;
            if (!fwd_seen[i] || !is_branch(d->op)) continue;
            if (d->rs2 == rv_ireg_zero) reg = d->rs1;
            else if (d->rs1 == rv_ireg_zero) reg = d->rs2;
            if (reg < 0) continue;
            if (stat_st[i][reg].owner == (int)sc_idx) {
                branch_idx = i;
                break;
            }
            if (stat_st[i][reg].owner >= 0) {
                reason_add(&c->reasons, &c->reason_count,
                    RV_LRSC_REASON_STATUS_FOREIGN, d->pc, 0, 0);
            } else {
                reason_add(&c->reasons, &c->reason_count,
                    RV_LRSC_REASON_NO_STATUS_BRANCH, d->pc, 0, 0);
            }
        }
        if (branch_idx == (size_t)-1) {
            reason_add(&c->reasons, &c->reason_count,
                RV_LRSC_REASON_NO_STATUS_BRANCH,
                blk->recs[sc_idx].dec.pc, 0, 0);
        }
    } else {
        reason_add(&c->reasons, &c->reason_count,
            RV_LRSC_REASON_NO_SC, blk->recs[lr_idx].dec.pc, 0, 0);
    }

    /* ---- branch edge identities (direction comes from the opcode,
     *      not the instruction layout) ---- */
    if (branch_idx != (size_t)-1) {
        const rv_decode *bd = &blk->recs[branch_idx].dec;
        size_t taken_node = (size_t)-1, fall_node = (size_t)-1;
        uint64_t target;
        if (rv_code_rec_branch_target(blk, branch_idx, &target) == 0) {
            long ti = rv_code_block_find(blk, target);
            if (ti >= 0) taken_node = (size_t)ti;
        }
        if (branch_idx + 1 < az->n) fall_node = branch_idx + 1;

        if (bd->op == rv_op_bne) {
            fail_node = taken_node;  /* non-zero status takes the branch */
            succ_node = fall_node;
        } else { /* rv_op_beq: zero status takes the branch */
            fail_node = fall_node;
            succ_node = taken_node;
        }
    }

    /* ---- phase B: build the exact retry region ---- */
    in_region = calloc(az->n, 1);
    if (!in_region) goto fail;
    if (branch_idx != (size_t)-1) {
        region_walk rw;
        memset(&rw, 0, sizeof(rw));
        rw.in_region = in_region;
        rw.lr_idx = lr_idx;
        rw.sc_idx = sc_idx;
        rw.branch_idx = branch_idx;
        rw.fail_node = fail_node;
        rw.succ_node = succ_node;
        rw.reasons = c->reasons;
        rw.nreasons = &c->reason_count;
        rw.az = az;
        region_dfs(&rw, lr_idx, 1);
        c->reasons = rw.reasons;
    } else {
        /* no status branch: the forward set is the best region we have */
        memcpy(in_region, fwd_seen, az->n);
    }

    for (i = lr_idx; i < az->n; i++) {
        if (in_region[i]) {
            region_count++;
            region_max = i;
        }
    }
    if (region_count > MAX_REGION) {
        reason_add(&c->reasons, &c->reason_count,
            RV_LRSC_REASON_REGION_TOO_LONG,
            blk->recs[region_max].dec.pc, 0, 0);
    }

    /* continuity: every index between LR and region_max belongs to it */
    for (i = lr_idx; i <= region_max; i++) {
        if (!in_region[i]) {
            reason_add(&c->reasons, &c->reason_count,
                RV_LRSC_REASON_UNREACHABLE_NODE,
                blk->recs[i].dec.pc, 0, 0);
        }
    }

    /* back-edges collected in phase A, judged against the region */
    for (i = 0; i < fw.nback; i++) {
        long oi = rv_code_block_find(blk, fw.back_from[i]);
        int from_inside = (oi >= 0 && (size_t)oi >= lr_idx &&
            (size_t)oi <= region_max && in_region[oi]);
        if (!from_inside) continue; /* edge on the success path */
        if (!fw.back_to_lr[i] ||
            (sc_idx != (size_t)-1 && (size_t)oi < sc_idx)) {
            reason_add(&c->reasons, &c->reason_count,
                RV_LRSC_REASON_BACKWARD_EDGE,
                fw.back_from[i], fw.back_from[i], fw.back_to[i]);
        }
    }

    /* no external entry may reach the region body */
    for (i = lr_idx + 1; i <= region_max; i++) {
        if (!in_region[i]) continue;
        if (has_pred_outside_region(az, in_region, i, entries,
                entry_count)) {
            reason_add(&c->reasons, &c->reason_count,
                RV_LRSC_REASON_EXTERNAL_ENTRY, blk->recs[i].dec.pc, 0, 0);
        }
    }

    /* ---- endpoint records and width ---- */
    c->lr_rec = blk->recs[lr_idx];
    c->width = (uint8_t)lrsc_width(blk->recs[lr_idx].dec.op);
    if (sc_idx != (size_t)-1) {
        c->sc_rec = blk->recs[sc_idx];
        if (lrsc_width(blk->recs[sc_idx].dec.op) != c->width) {
            reason_add(&c->reasons, &c->reason_count,
                RV_LRSC_REASON_WIDTH_MISMATCH,
                blk->recs[sc_idx].dec.pc, 0, 0);
        }
    }

    /* ---- retry / success certificates ---- */
    if (branch_idx != (size_t)-1) {
        const rv_decode *bd = &blk->recs[branch_idx].dec;
        c->branch_rec = blk->recs[branch_idx];
        c->fail_edge_from = bd->pc;
        c->success_edge_from = bd->pc;
        c->fail_edge_to = (fail_node < az->n) ?
            blk->recs[fail_node].dec.pc : 0;
        c->success_edge_to = (succ_node < az->n) ?
            blk->recs[succ_node].dec.pc : 0;

        if (fail_node >= az->n || succ_node >= az->n ||
            trace_tail(az, fail_node, 1, lr_idx, in_region, region_max,
                &c->fail_edge_from, &c->fail_edge_to) != 0 ||
            trace_tail(az, succ_node, 0, lr_idx, in_region, region_max,
                &c->success_edge_from, &c->success_edge_to) != 0) {
            reason_add(&c->reasons, &c->reason_count,
                RV_LRSC_REASON_EDGE_UNPROVEN, bd->pc, 0, 0);
            if (c->fail_edge_to != blk->recs[lr_idx].dec.pc)
                c->fail_edge_to = 0;
        }
    }

    /* ---- region description (PC order, stable) ---- */
    c->region_start_pc = blk->recs[lr_idx].dec.pc;
    c->region_end_pc = blk->recs[region_max].dec.pc;
    for (i = lr_idx; i <= region_max && c->region_count < MAX_REGION; i++) {
        if (in_region[i])
            c->region_pcs[c->region_count++] = blk->recs[i].dec.pc;
    }

    /* ---- verdict ---- */
    for (i = 0; i < c->reason_count; i++) {
        switch (c->reasons[i].kind) {
        case RV_LRSC_REASON_INTERVENING_LR:
        case RV_LRSC_REASON_WIDTH_MISMATCH:
        case RV_LRSC_REASON_FORBIDDEN_INSN:
        case RV_LRSC_REASON_BACKWARD_EDGE:
        case RV_LRSC_REASON_REGION_TOO_LONG:
        case RV_LRSC_REASON_EXTERNAL_ENTRY:
        case RV_LRSC_REASON_UNREACHABLE_NODE:
            hard_violation = 1;
            break;
        default:
            break;
        }
    }
    if (hard_violation) {
        c->verdict = RV_LRSC_VERDICT_UNCONSTRAINED;
    } else if (sc_idx != (size_t)-1 && branch_idx != (size_t)-1 &&
               c->fail_edge_to == blk->recs[lr_idx].dec.pc &&
               c->success_edge_to != 0 &&
               c->success_edge_to != blk->recs[lr_idx].dec.pc) {
        int addr_ok = 1;
        for (i = 0; i < c->reason_count; i++) {
            if (c->reasons[i].kind == RV_LRSC_REASON_ADDR_MISMATCH ||
                c->reasons[i].kind == RV_LRSC_REASON_ADDR_UNKNOWN)
                addr_ok = 0;
        }
        if (addr_ok) c->verdict = RV_LRSC_VERDICT_PROVEN;
    }

    free(fwd_seen);
    free(in_region);
    free(addr_st);
    free(stat_st);
    free(fw.sc_nodes);
    free(fw.back_from);
    free(fw.back_to);
    free(fw.back_to_lr);
    return 0;

fail:
    free(fwd_seen);
    free(in_region);
    free(addr_st);
    free(stat_st);
    free(fw.sc_nodes);
    free(fw.back_from);
    free(fw.back_to);
    free(fw.back_to_lr);
    return -1;
}

/* ------------------------------------------------------------------ */
/* public entry point                                                 */
/* ------------------------------------------------------------------ */

rv_lrsc_result *rv_lrsc_analyze(const rv_code_block *blk,
    const uint64_t *entries, size_t entry_count)
{
    struct analyzer az;
    rv_lrsc_result *res;
    size_t i, k, ncand = 0;

    if (!blk || !entries || entry_count == 0) return NULL;
    for (i = 0; i < entry_count; i++) {
        if (rv_code_block_find(blk, entries[i]) < 0) return NULL;
    }

    memset(&az, 0, sizeof(az));
    az.blk = blk;
    az.n = blk->count;
    az.info = calloc(az.n, sizeof(*az.info));
    if (!az.info) return NULL;
    build_cfg(&az);

    for (i = 0; i < entry_count; i++) {
        long ei = rv_code_block_find(blk, entries[i]);
        reach_dfs(&az, (size_t)ei);
    }

    /* block-level structural evidence */
    for (i = 0; i < az.n; i++) {
        const rv_code_rec *r = &blk->recs[i];
        if (!az.info[i].reached) continue;
        if (is_region_forbidden(r->dec.op) && !is_lr(r->dec.op) &&
            !is_sc(r->dec.op)) {
            add_violation(&az, RV_LRSC_REASON_FORBIDDEN_INSN,
                r->dec.pc, 0, 0);
        }
        for (k = 0; k < az.info[i].nsucc; k++) {
            size_t v = az.info[i].succ[k];
            if (v <= i) {
                add_violation(&az, RV_LRSC_REASON_BACKWARD_EDGE,
                    r->dec.pc, r->dec.pc, blk->recs[v].dec.pc);
            }
        }
    }

    res = calloc(1, sizeof(*res));
    if (!res) goto out;
    res->entries = malloc(entry_count * sizeof(uint64_t));
    if (!res->entries) { free(res); res = NULL; goto out; }
    memcpy(res->entries, entries, entry_count * sizeof(uint64_t));
    res->entry_count = entry_count;

    for (i = 0; i < az.n; i++)
        if (az.info[i].reached) res->reached_count++;

    res->candidates = malloc(az.n * sizeof(*res->candidates));
    if (!res->candidates) {
        free(res->entries); free(res); res = NULL; goto out;
    }

    for (i = 0; i < az.n; i++) {
        if (!az.info[i].reached || !is_lr(blk->recs[i].dec.op)) continue;
        if (analyze_lr(&az, i, entries, entry_count,
                &res->candidates[ncand]) != 0) {
            size_t j;
            for (j = 0; j <= ncand; j++)
                free(res->candidates[j].reasons);
            free(res->candidates);
            free(res->entries);
            free(res);
            res = NULL;
            goto out;
        }
        ncand++;
    }
    res->candidate_count = ncand;

    res->violations = az.violations;
    res->violation_count = az.nviolations;
    az.violations = NULL;

out:
    free(az.violations);
    free(az.info);
    return res;
}

void rv_lrsc_result_free(rv_lrsc_result *res)
{
    size_t i;
    if (!res) return;
    free(res->entries);
    for (i = 0; i < res->candidate_count; i++)
        free(res->candidates[i].reasons);
    free(res->candidates);
    free(res->violations);
    free(res);
}

const char *rv_lrsc_verdict_name(rv_lrsc_verdict v)
{
    switch (v) {
    case RV_LRSC_VERDICT_PROVEN: return "proven";
    case RV_LRSC_VERDICT_UNCONSTRAINED: return "unconstrained";
    default: return "unknown";
    }
}

const char *rv_lrsc_reason_name(rv_lrsc_reason_kind k)
{
    switch (k) {
    case RV_LRSC_REASON_INTERVENING_LR: return "intervening-lr";
    case RV_LRSC_REASON_WIDTH_MISMATCH: return "width-mismatch";
    case RV_LRSC_REASON_FORBIDDEN_INSN: return "forbidden-insn";
    case RV_LRSC_REASON_BACKWARD_EDGE: return "backward-edge";
    case RV_LRSC_REASON_REGION_TOO_LONG: return "region-too-long";
    case RV_LRSC_REASON_EXTERNAL_ENTRY: return "external-entry";
    case RV_LRSC_REASON_UNREACHABLE_NODE: return "unreachable-node";
    case RV_LRSC_REASON_NO_SC: return "no-sc";
    case RV_LRSC_REASON_ADDR_MISMATCH: return "addr-mismatch";
    case RV_LRSC_REASON_ADDR_UNKNOWN: return "addr-unknown";
    case RV_LRSC_REASON_STATUS_CLOBBERED: return "status-clobbered";
    case RV_LRSC_REASON_STATUS_FOREIGN: return "status-foreign";
    case RV_LRSC_REASON_NO_STATUS_BRANCH: return "no-status-branch";
    case RV_LRSC_REASON_EDGE_UNPROVEN: return "edge-unproven";
    default: return "none";
    }
}
