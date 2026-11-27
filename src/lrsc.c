/*
 * Static LR/SC retry-region analysis.
 *
 * All decoding goes through rv_decode_inst() (the same production path used
 * by disasm_inst); this file never re-extracts instruction bit fields and
 * never parses assembler text. The input image is copied once and only the
 * private copy is inspected. Nothing is executed.
 */
#include "lrsc.h"

/* ------------------------------------------------------------------ */
/* internal value types                                                */
/* ------------------------------------------------------------------ */

#define LRSC_MAX_REGION 64   /* exploration cap; certificates cap at 16 */

struct lrsc_report {
    uint8_t *code;          /* owned copy */
    size_t code_len;
    uint64_t base_pc;
    rv_isa isa;
    lrsc_node *nodes;       /* in linear sweep order, PC ascending */
    size_t nnodes;
    int32_t *offset_map;    /* byte offset -> node index, -1 if interior */
    int **preds;
    size_t *npred;
    size_t reachable;
    lrsc_candidate *cand;
    size_t ncand;
};

/* symbolic address token: entry-register identity plus constant offset */
enum { SYM_BOTTOM = 0, SYM_KNOWN, SYM_UNKNOWN };

typedef struct {
    uint8_t kind;
    int8_t base;            /* entry register number, -1 for pure constant */
    int32_t off;
} sym_addr;

/* status token: which SC produced the value held in a register */
enum { ST_NONE = -1, ST_CONFLICT = -2 };

typedef struct {
    int producer;           /* SC node index, ST_NONE, or ST_CONFLICT */
} sym_status;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static sym_addr sym_identity(unsigned r)
{
    sym_addr s = { SYM_KNOWN, (int8_t)r, 0 };
    return s;
}

static sym_addr sym_const(int32_t v)
{
    sym_addr s = { SYM_KNOWN, -1, v };
    return s;
}

static bool sym_equal(const sym_addr *a, const sym_addr *b)
{
    return a->kind == b->kind && a->base == b->base && a->off == b->off;
}

static sym_addr sym_join(const sym_addr *a, const sym_addr *b)
{
    if (a->kind == SYM_BOTTOM) return *b;
    if (b->kind == SYM_BOTTOM) return *a;
    if (a->kind == SYM_UNKNOWN || b->kind == SYM_UNKNOWN) {
        sym_addr u = { SYM_UNKNOWN, -1, 0 };
        return u;
    }
    if (sym_equal(a, b)) return *a;
    /* different concrete addresses on different paths must not be merged */
    sym_addr u = { SYM_UNKNOWN, -1, 0 };
    return u;
}

static int cmp_index(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb;
    return (a > b) - (a < b);
}

static bool is_lr_op(rv_opcode op)
{
    return op == rv_op_lr_w || op == rv_op_lr_d;
}

static bool is_sc_op(rv_opcode op)
{
    return op == rv_op_sc_w || op == rv_op_sc_d;
}

static bool same_width(rv_opcode a, rv_opcode b)
{
    return (a == rv_op_lr_w && b == rv_op_sc_w) ||
           (a == rv_op_lr_d && b == rv_op_sc_d);
}

static bool is_branch_op(rv_opcode op)
{
    switch (op) {
    case rv_op_beq: case rv_op_bne: case rv_op_blt: case rv_op_bge:
    case rv_op_bltu: case rv_op_bgeu:
        return true;
    default:
        return false;
    }
}

/* arithmetic/logic allowed by RV64 I inside a retry region */
static bool is_allowed_alu(rv_opcode op)
{
    switch (op) {
    case rv_op_lui: case rv_op_auipc:
    case rv_op_addi: case rv_op_slti: case rv_op_sltiu:
    case rv_op_xori: case rv_op_ori: case rv_op_andi:
    case rv_op_slli: case rv_op_srli: case rv_op_srai:
    case rv_op_add: case rv_op_sub: case rv_op_sll: case rv_op_slt:
    case rv_op_sltu: case rv_op_xor: case rv_op_srl: case rv_op_sra:
    case rv_op_or: case rv_op_and:
    case rv_op_addiw: case rv_op_slliw: case rv_op_srliw: case rv_op_sraiw:
    case rv_op_addw: case rv_op_subw: case rv_op_sllw:
    case rv_op_srlw: case rv_op_sraw:
        return true;
    default:
        return false;
    }
}

static bool is_load_store_amo(rv_opcode op)
{
    switch (op) {
    case rv_op_lb: case rv_op_lh: case rv_op_lw: case rv_op_lbu:
    case rv_op_lhu: case rv_op_lwu: case rv_op_ld:
    case rv_op_sb: case rv_op_sh: case rv_op_sw: case rv_op_sd:
    case rv_op_amoswap_w: case rv_op_amoadd_w: case rv_op_amoxor_w:
    case rv_op_amoor_w: case rv_op_amoand_w: case rv_op_amomin_w:
    case rv_op_amomax_w: case rv_op_amominu_w: case rv_op_amomaxu_w:
    case rv_op_amoswap_d: case rv_op_amoadd_d: case rv_op_amoxor_d:
    case rv_op_amoor_d: case rv_op_amoand_d: case rv_op_amomin_d:
    case rv_op_amomax_d: case rv_op_amominu_d: case rv_op_amomaxu_d:
        return true;
    default:
        return false;
    }
}

static bool is_muldiv(rv_opcode op)
{
    switch (op) {
    case rv_op_mul: case rv_op_mulh: case rv_op_mulhsu: case rv_op_mulhu:
    case rv_op_div: case rv_op_divu: case rv_op_rem: case rv_op_remu:
    case rv_op_mulw: case rv_op_divw: case rv_op_divuw:
    case rv_op_remw: case rv_op_remuw:
        return true;
    default:
        return false;
    }
}

static bool is_system(rv_opcode op)
{
    switch (op) {
    case rv_op_ecall: case rv_op_ebreak:
    case rv_op_uret: case rv_op_sret: case rv_op_hret: case rv_op_mret:
    case rv_op_dret: case rv_op_sfence_vm: case rv_op_sfence_vma:
    case rv_op_wfi:
    case rv_op_csrrw: case rv_op_csrrs: case rv_op_csrrc:
    case rv_op_csrrwi: case rv_op_csrrsi: case rv_op_csrrci:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* decode + linear sweep                                               */
/* ------------------------------------------------------------------ */

static int32_t report_index_at(const lrsc_report *r, uint64_t pc)
{
    if (pc < r->base_pc) return -1;
    uint64_t off = pc - r->base_pc;
    if (off >= r->code_len) return -1;
    return r->offset_map[off];
}

static void build_successors(lrsc_report *r, size_t ni)
{
    lrsc_node *n = &r->nodes[ni];
    const rv_decode *d = &n->dec;
    uint64_t next_pc = d->pc + n->len;
    rv_opcode op = d->op;

    if (op == rv_op_illegal)
        return;

    if (is_branch_op(op)) {
        n->succ[n->nsucc].pc = next_pc;
        n->succ[n->nsucc].kind = LRSC_EDGE_FALLTHROUGH;
        n->nsucc++;
        n->succ[n->nsucc].pc = d->pc + (uint64_t)(int64_t)d->imm;
        n->succ[n->nsucc].kind = LRSC_EDGE_BRANCH;
        n->nsucc++;
    } else if (op == rv_op_jal) {
        /* c.j and j lift only in the printer; here op is still jal */
        n->succ[n->nsucc].pc = d->pc + (uint64_t)(int64_t)d->imm;
        n->succ[n->nsucc].kind = LRSC_EDGE_JUMP;
        n->nsucc++;
    } else if (op == rv_op_jalr) {
        /* indirect transfer: no recoverable direct successor */
        return;
    } else {
        n->succ[n->nsucc].pc = next_pc;
        n->succ[n->nsucc].kind = LRSC_EDGE_FALLTHROUGH;
        n->nsucc++;
    }

    for (unsigned k = 0; k < n->nsucc; k++) {
        if (report_index_at(r, n->succ[k].pc) < 0)
            n->flags |= LRSC_NODE_EDGE_OUT;
    }
}

static int sweep(lrsc_report *r)
{
    size_t off = 0, cap = 0;
    r->nodes = NULL;
    r->offset_map = malloc(r->code_len * sizeof(r->offset_map[0]));
    if (!r->offset_map) return -1;
    for (size_t i = 0; i < r->code_len; i++) r->offset_map[i] = -1;

    while (off < r->code_len) {
        rv_inst inst;
        size_t ilen;
        if (r->code_len - off < 2) return -1;
        inst_fetch(r->code + off, &inst, &ilen);
        if (ilen != 2 && ilen != 4) return -1;
        if (off + ilen > r->code_len) return -1;

        if (r->nnodes == cap) {
            cap = cap ? cap * 2 : 16;
            lrsc_node *grow = realloc(r->nodes, cap * sizeof(*grow));
            if (!grow) return -1;
            r->nodes = grow;
        }
        lrsc_node *n = &r->nodes[r->nnodes];
        memset(n, 0, sizeof(*n));
        n->len = (uint16_t)ilen;
        rv_decode_inst(r->isa, r->base_pc + off, inst, &n->dec);
        r->offset_map[off] = (int32_t)r->nnodes;
        r->nnodes++;
        off += ilen;
    }

    for (size_t i = 0; i < r->nnodes; i++)
        build_successors(r, i);
    return 0;
}

/* predecessor adjacency over direct, in-image edges */
static int build_predecessors(lrsc_report *r)
{
    r->preds = calloc(r->nnodes, sizeof(*r->preds));
    r->npred = calloc(r->nnodes, sizeof(*r->npred));
    if (!r->preds || !r->npred) return -1;

    for (size_t i = 0; i < r->nnodes; i++) {
        const lrsc_node *n = &r->nodes[i];
        for (unsigned k = 0; k < n->nsucc; k++) {
            int32_t t = report_index_at(r, n->succ[k].pc);
            if (t < 0) continue;
            size_t cnt = r->npred[t];
            if ((cnt & 7u) == 0) {
                int *grow = realloc(r->preds[t], (cnt + 8) * sizeof(int));
                if (!grow) return -1;
                r->preds[t] = grow;
            }
            r->preds[t][cnt] = (int)i;
            r->npred[t] = cnt + 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* reachability from the entry set                                     */
/* ------------------------------------------------------------------ */

static int mark_reachable(lrsc_report *r, const uint64_t *entries,
                          size_t nentries)
{
    int *stack = malloc(r->nnodes * sizeof(int));
    if (!stack) return -1;
    size_t sp = 0;

    for (size_t e = 0; e < nentries; e++) {
        int32_t idx = report_index_at(r, entries[e]);
        if (idx < 0) { free(stack); return -1; }
        lrsc_node *n = &r->nodes[idx];
        n->flags |= LRSC_NODE_ENTRY;
        if (!(n->flags & LRSC_NODE_REACHABLE)) {
            n->flags |= LRSC_NODE_REACHABLE;
            stack[sp++] = idx;
        }
    }

    while (sp) {
        int cur = stack[--sp];
        const lrsc_node *n = &r->nodes[cur];
        r->reachable++;
        for (unsigned k = 0; k < n->nsucc; k++) {
            int32_t t = report_index_at(r, n->succ[k].pc);
            if (t < 0) continue;
            lrsc_node *s = &r->nodes[t];
            if (!(s->flags & LRSC_NODE_REACHABLE)) {
                s->flags |= LRSC_NODE_REACHABLE;
                stack[sp++] = t;
            }
        }
    }
    free(stack);
    return 0;
}

/* ------------------------------------------------------------------ */
/* symbolic address flow (entry-register identities, whole reachable)  */
/* ------------------------------------------------------------------ */

static void addr_transfer(const rv_decode *d, const sym_addr *in, sym_addr *out)
{
    memcpy(out, in, 32 * sizeof(sym_addr));
    unsigned rd = d->rd;
    if (rd == 0) return; /* x0 is never redefined */

    switch (d->op) {
    case rv_op_addi: /* covers c.addi, c.mv, c.li, c.addi4spn, c.addi16sp */
        if (in[d->rs1].kind == SYM_KNOWN) {
            out[rd].kind = SYM_KNOWN;
            out[rd].base = in[d->rs1].base;
            out[rd].off = in[d->rs1].off + d->imm;
        } else {
            out[rd].kind = SYM_UNKNOWN;
        }
        break;
    case rv_op_add: { /* covers c.add; x0 copies are identity */
        sym_addr a = in[d->rs1], b = in[d->rs2];
        if (d->rs2 == 0) {
            out[rd] = a;
        } else if (d->rs1 == 0) {
            out[rd] = b;
        } else if (a.kind == SYM_KNOWN && b.kind == SYM_KNOWN &&
                   a.base == b.base) {
            out[rd].kind = SYM_KNOWN;
            out[rd].base = a.base;
            out[rd].off = a.off + b.off;
        } else {
            out[rd].kind = SYM_UNKNOWN;
        }
        break;
    }
    default:
        out[rd].kind = SYM_UNKNOWN;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* per-candidate region exploration                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int idx[LRSC_MAX_REGION];
    size_t count;
    bool truncated;
} region_set;

static bool region_contains(const region_set *s, int node)
{
    for (size_t i = 0; i < s->count; i++)
        if (s->idx[i] == node) return true;
    return false;
}

static void region_add(region_set *s, int node)
{
    if (region_contains(s, node)) return;
    if (s->count < LRSC_MAX_REGION)
        s->idx[s->count] = node;
    else
        s->truncated = true;
    s->count++;
}

/* BFS inside one LR's forward subgraph; back edges to the LR are recorded
 * via predecessor checks later, not expanded. */
static region_set explore_region(lrsc_report *r, int lr_idx)
{
    region_set s = { .count = 0, .truncated = false };
    int stack[LRSC_MAX_REGION];
    size_t sp = 0;
    region_add(&s, lr_idx);
    stack[sp++] = lr_idx;

    while (sp) {
        int cur = stack[--sp];
        const lrsc_node *n = &r->nodes[cur];
        for (unsigned k = 0; k < n->nsucc; k++) {
            int32_t t = report_index_at(r, n->succ[k].pc);
            if (t < 0) continue;                       /* edge-out handled later */
            if (t == lr_idx) continue;                /* retry edge, not expanded */
            if (region_contains(&s, t)) continue;
            region_add(&s, t);
            if (!s.truncated && sp < LRSC_MAX_REGION)
                stack[sp++] = t;
        }
    }
    qsort(s.idx, s.count < LRSC_MAX_REGION ? s.count : LRSC_MAX_REGION,
          sizeof(int), cmp_index);
    return s;
}

/* ------------------------------------------------------------------ */
/* address certification inside one region                             */
/* ------------------------------------------------------------------ */

typedef struct {
    lrsc_reason reason;
    uint64_t reason_pc;
} addr_result;

/* last instruction in the region that writes reg (linear order) */
static int last_writer(lrsc_report *r, const region_set *s, unsigned reg,
                       int before)
{
    int found = -1;
    size_t lim = s->count < LRSC_MAX_REGION ? s->count : LRSC_MAX_REGION;
    for (size_t i = 0; i < lim; i++) {
        int ni = s->idx[i];
        if (ni >= before) break;
        if (r->nodes[ni].dec.rd == reg && reg != 0) found = ni;
    }
    return found;
}

static addr_result certify_address(lrsc_report *r, const region_set *s,
                                   int lr_idx, int sc_idx)
{
    addr_result res = { LRSC_REASON_NONE, 0 };
    const rv_decode *lr = &r->nodes[lr_idx].dec;
    const rv_decode *sc = &r->nodes[sc_idx].dec;

    /* forward interpretation over the sorted region nodes. The LR is seeded
     * with entry-register identities; a retry edge arriving at the LR is the
     * loop boundary, not a second definition, so it is never merged in. */
    sym_addr state[LRSC_MAX_REGION][32];
    sym_addr entry_state[LRSC_MAX_REGION][32];
    bool have[LRSC_MAX_REGION] = { false };
    size_t lim = s->count < LRSC_MAX_REGION ? s->count : LRSC_MAX_REGION;

    for (size_t i = 0; i < lim; i++) {
        int ni = s->idx[i];
        sym_addr entry[32];
        bool any = false;
        for (size_t p = 0; p < r->npred[ni]; p++) {
            int pi = r->preds[ni][p];
            if (ni == lr_idx) continue;       /* LR entry is its own seed */
            if (!region_contains(s, pi)) continue;
            size_t slot = 0;
            while (slot < lim && s->idx[slot] != pi) slot++;
            if (slot >= lim || !have[slot]) continue;
            if (!any) { memcpy(entry, state[slot], sizeof(entry)); any = true; }
            else
                for (unsigned reg = 0; reg < 32; reg++)
                    entry[reg] = sym_join(&entry[reg], &state[slot][reg]);
        }
        if (!any) {
            if (ni == lr_idx) {
                for (unsigned reg = 0; reg < 32; reg++)
                    entry[reg] = reg == 0 ? sym_const(0) : sym_identity(reg);
            } else {
                continue; /* unreachable within region; skip */
            }
        }
        have[i] = true;
        memcpy(entry_state[i], entry, sizeof(entry));
        addr_transfer(&r->nodes[ni].dec, entry, state[i]);
    }

    /* token held by the SC address register on entry to the SC: the SC
     * reads its address before defining its result register */
    size_t sc_slot = 0;
    while (sc_slot < lim && s->idx[sc_slot] != sc_idx) sc_slot++;
    if (sc_slot >= lim || !have[sc_slot]) {
        res.reason = LRSC_REASON_ADDR_UNKNOWN;
        res.reason_pc = sc->pc;
        return res;
    }

    size_t lr_slot = 0;
    while (lr_slot < lim && s->idx[lr_slot] != lr_idx) lr_slot++;
    sym_addr lr_token = (lr_slot < lim && have[lr_slot])
        ? entry_state[lr_slot][lr->rs1] : sym_identity(lr->rs1);
    sym_addr sc_token = entry_state[sc_slot][sc->rs1];

    if (sc_token.kind == SYM_KNOWN && lr_token.kind == SYM_KNOWN &&
        sym_equal(&sc_token, &lr_token))
        return res;

    /* prove why the association failed */
    int w = last_writer(r, s, sc->rs1, sc_idx);
    if (sc_token.kind == SYM_UNKNOWN) {
        /* a join inside the region that lost the token? */
        for (size_t i = 0; i < lim; i++) {
            int ni = s->idx[i];
            if (ni == lr_idx || !have[i]) continue;
            unsigned nreg = 0;
            for (size_t p = 0; p < r->npred[ni]; p++) {
                int pi = r->preds[ni][p];
                if (!region_contains(s, pi) || pi == lr_idx) continue;
                size_t slot = 0;
                while (slot < lim && s->idx[slot] != pi) slot++;
                if (slot < lim && have[slot]) nreg++;
            }
            if (nreg >= 2) {
                res.reason = LRSC_REASON_ADDR_MERGE;
                res.reason_pc = r->nodes[ni].dec.pc;
                return res;
            }
        }
        res.reason = LRSC_REASON_ADDR_OVERWRITTEN;
        res.reason_pc = w >= 0 ? r->nodes[w].dec.pc : sc->pc;
        return res;
    }

    /* concrete but different address: the register was redefined */
    res.reason = LRSC_REASON_ADDR_OVERWRITTEN;
    res.reason_pc = w >= 0 ? r->nodes[w].dec.pc : sc->pc;
    return res;
}

/* ------------------------------------------------------------------ */
/* SC status flow and branch certification                             */
/* ------------------------------------------------------------------ */

static void status_transfer(const rv_decode *d, int sc_idx,
                            const sym_status *in, sym_status *out)
{
    memcpy(out, in, 32 * sizeof(sym_status));
    unsigned rd = d->rd;
    if (rd == 0) return;

    if (is_sc_op(d->op)) {
        out[rd].producer = (int)sc_idx; /* address read happens before this def */
        return;
    }
    switch (d->op) {
    case rv_op_addi: /* imm == 0 forms are the lossless copies (mv, c.mv) */
        if (d->imm == 0)
            out[rd].producer = in[d->rs1].producer;
        else
            out[rd].producer = ST_NONE;
        break;
    case rv_op_add:
        if (d->rs2 == 0)
            out[rd].producer = in[d->rs1].producer;
        else if (d->rs1 == 0)
            out[rd].producer = in[d->rs2].producer;
        else
            out[rd].producer = ST_NONE;
        break;
    default:
        out[rd].producer = ST_NONE;
        break;
    }
}

/* Merge two statuses that both come from real contributing predecessors.
 * ST_NONE here means that path overwrote the SC result, so equivalence on
 * only the other predecessor must not be promoted into a proof. */
static int status_join(int a, int b)
{
    if (a == ST_NONE || b == ST_NONE) return ST_CONFLICT;
    return a == b ? a : ST_CONFLICT;
}

/* follow an unconditional JAL x0 / c.j chain to its final target */
static uint64_t follow_jump(lrsc_report *r, uint64_t pc)
{
    for (unsigned hops = 0; hops < LRSC_MAX_REGION; hops++) {
        int32_t idx = report_index_at(r, pc);
        if (idx < 0) return pc;
        const lrsc_node *n = &r->nodes[idx];
        if (n->dec.op != rv_op_jal || n->dec.rd != 0) return pc;
        pc = n->dec.pc + (uint64_t)(int64_t)n->dec.imm;
    }
    return pc;
}

typedef struct {
    lrsc_reason reason;
    uint64_t reason_pc;
    uint64_t branch_pc;
    uint64_t fail_target;
    uint64_t success_target;
} status_result;

static status_result certify_status(lrsc_report *r, const region_set *s,
                                    int lr_idx, int sc_idx)
{
    status_result res = {
        LRSC_REASON_NO_STATUS_BRANCH, 0, 0, 0, 0
    };
    size_t lim = s->count < LRSC_MAX_REGION ? s->count : LRSC_MAX_REGION;
    sym_status state[LRSC_MAX_REGION][32];
    bool have[LRSC_MAX_REGION] = { false };

    for (size_t i = 0; i < lim; i++) {
        int ni = s->idx[i];
        if (ni < sc_idx) continue;

        sym_status entry[32];
        for (unsigned reg = 0; reg < 32; reg++) entry[reg].producer = ST_NONE;
        bool any = false;
        for (size_t p = 0; p < r->npred[ni]; p++) {
            int pi = r->preds[ni][p];
            if (!region_contains(s, pi) || pi < sc_idx) continue;
            size_t slot = 0;
            while (slot < lim && s->idx[slot] != pi) slot++;
            if (slot >= lim || !have[slot]) continue;
            if (!any) { memcpy(entry, state[slot], sizeof(entry)); any = true; }
            else
                for (unsigned reg = 0; reg < 32; reg++)
                    entry[reg].producer = status_join(entry[reg].producer,
                                                      state[slot][reg].producer);
        }
        if (ni != sc_idx && !any) continue;
        have[i] = true;
        status_transfer(&r->nodes[ni].dec, sc_idx, entry, state[i]);

        const rv_decode *d = &r->nodes[ni].dec;
        /* only BEQ/BNE (including c.beqz/c.bnez, which decode to those ops)
         * against x0 are valid zero/nonzero status tests */
        if (d->op != rv_op_beq && d->op != rv_op_bne) continue;

        unsigned status_reg = 0;
        bool zero_compare = false;
        if (d->rs2 == 0) { status_reg = d->rs1; zero_compare = true; }
        else if (d->rs1 == 0) { status_reg = d->rs2; zero_compare = true; }
        if (!zero_compare) continue;

        int producer = state[i][status_reg].producer;
        if (producer != sc_idx) {
            if (producer == ST_CONFLICT) {
                res.reason = LRSC_REASON_STATUS_MERGE;
                res.reason_pc = d->pc;
            } else if (producer >= 0) {
                res.reason = LRSC_REASON_STATUS_FOREIGN;
                res.reason_pc = d->pc;
            } else {
                res.reason = LRSC_REASON_STATUS_LOST;
                res.reason_pc = d->pc;
            }
            continue;
        }

        uint64_t taken = follow_jump(r, d->pc + (uint64_t)(int64_t)d->imm);
        uint64_t fall = follow_jump(r, d->pc + r->nodes[ni].len);
        uint64_t fail_pc, ok_pc;
        if (d->op == rv_op_bne) { fail_pc = taken; ok_pc = fall; }
        else                     { fail_pc = fall;  ok_pc = taken; } /* beq */

        res.branch_pc = d->pc;
        res.fail_target = fail_pc;
        res.success_target = ok_pc;

        /* only the failure edge is judged here; whether the success edge
         * actually leaves the region is checked after the region is trimmed */
        if (fail_pc != r->nodes[lr_idx].dec.pc) {
            res.reason = LRSC_REASON_FAIL_EDGE;
            res.reason_pc = d->pc;
        } else {
            res.reason = LRSC_REASON_NONE;
            res.reason_pc = 0;
        }
        return res;
    }
    return res;
}

/* ------------------------------------------------------------------ */
/* structural classification of a region's instructions and edges      */
/* ------------------------------------------------------------------ */

static lrsc_reason classify_op(const rv_decode *d, uint64_t *reason_pc)
{
    rv_opcode op = d->op;
    if (op == rv_op_illegal) { *reason_pc = d->pc; return LRSC_REASON_ILLEGAL_INST; }
    if (op == rv_op_fence || op == rv_op_fence_i) {
        *reason_pc = d->pc; return LRSC_REASON_FENCE;
    }
    if (is_system(op)) { *reason_pc = d->pc; return LRSC_REASON_SYSTEM; }
    if (is_muldiv(op)) { *reason_pc = d->pc; return LRSC_REASON_MULDIV; }
    if (is_load_store_amo(op)) { *reason_pc = d->pc; return LRSC_REASON_MEMORY_OP; }
    if (is_allowed_alu(op) || is_branch_op(op) || op == rv_op_jal)
        return LRSC_REASON_NONE;
    /* jalr and anything else unsupported inside a region */
    *reason_pc = d->pc;
    return LRSC_REASON_FORBIDDEN_OP;
}

static lrsc_reason structural_scan(lrsc_report *r, const region_set *s,
                                   int lr_idx, int sc_idx,
                                   uint64_t *reason_pc)
{
    size_t lim = s->count < LRSC_MAX_REGION ? s->count : LRSC_MAX_REGION;

    if (s->truncated || s->count > LRSC_CERT_REGION_MAX) {
        *reason_pc = r->nodes[lr_idx].dec.pc;
        return LRSC_REASON_REGION_TOO_LONG;
    }

    for (size_t i = 0; i < lim; i++) {
        int ni = s->idx[i];
        const lrsc_node *n = &r->nodes[ni];
        const rv_decode *d = &n->dec;

        /* edge direction: only forward edges, retry edge to LR excepted */
        for (unsigned k = 0; k < n->nsucc; k++) {
            int32_t t = report_index_at(r, n->succ[k].pc);
            if (t < 0) continue;
            if (t == lr_idx) {
                if (ni < sc_idx) { *reason_pc = d->pc; return LRSC_REASON_LOOP; }
                continue; /* retry tail edge, certified separately */
            }
            if (r->nodes[t].dec.pc <= d->pc) {
                *reason_pc = d->pc;
                return LRSC_REASON_BACKWARD_EDGE;
            }
        }

        /* no entries from outside the region except the LR itself */
        if (ni != lr_idx && (n->flags & LRSC_NODE_ENTRY)) {
            *reason_pc = d->pc;
            return LRSC_REASON_EXTERNAL_ENTRY;
        }
        if (ni != lr_idx) {
            for (size_t p = 0; p < r->npred[ni]; p++) {
                int pi = r->preds[ni][p];
                const lrsc_node *pn = &r->nodes[pi];
                if (!(pn->flags & LRSC_NODE_REACHABLE)) continue;
                if (!region_contains(s, pi)) {
                    *reason_pc = d->pc;
                    return LRSC_REASON_EXTERNAL_ENTRY;
                }
            }
        }

        if (ni == lr_idx || ni == sc_idx) continue; /* the pair itself */

        if (is_lr_op(d->op)) { *reason_pc = d->pc; return LRSC_REASON_INTERVENING_LR; }
        if (is_sc_op(d->op)) { *reason_pc = d->pc; return LRSC_REASON_FOREIGN_SC; }

        lrsc_reason why = classify_op(d, reason_pc);
        if (why != LRSC_REASON_NONE) return why;
    }
    return LRSC_REASON_NONE;
}

/* ------------------------------------------------------------------ */
/* Remove the certified success edge: everything reachable only through it is
 * success-tail code outside the retry region. Retry edges back to the LR are
 * not expanded, matching explore_region(). */
static region_set trim_success_path(lrsc_report *r, const region_set *full,
                                    int lr_idx, uint64_t success_pc)
{
    region_set out = { .count = 0, .truncated = false };
    int32_t excluded = report_index_at(r, success_pc);
    if (excluded < 0 || !region_contains(full, excluded))
        excluded = -1;

    int stack[LRSC_MAX_REGION];
    size_t sp = 0;
    region_add(&out, lr_idx);
    stack[sp++] = lr_idx;

    while (sp) {
        int cur = stack[--sp];
        const lrsc_node *n = &r->nodes[cur];
        for (unsigned k = 0; k < n->nsucc; k++) {
            int32_t t = report_index_at(r, n->succ[k].pc);
            if (t < 0 || t == lr_idx || t == excluded) continue;
            if (!region_contains(full, t)) continue;
            if (region_contains(&out, t)) continue;
            region_add(&out, t);
            if (!out.truncated && sp < LRSC_MAX_REGION)
                stack[sp++] = t;
        }
    }
    qsort(out.idx, out.count < LRSC_MAX_REGION ? out.count : LRSC_MAX_REGION,
          sizeof(int), cmp_index);
    return out;
}

/* candidate construction                                              */
/* ------------------------------------------------------------------ */

static void set_reason(lrsc_candidate *c, lrsc_reason reason, uint64_t pc,
                       bool hard)
{
    if (hard) {
        /* a structural violation dominates any dataflow uncertainty */
        if (c->verdict != LRSC_UNCONSTRAINED) {
            c->reason = reason;
            c->reason_pc = pc;
        }
        c->verdict = LRSC_UNCONSTRAINED;
    } else {
        if (c->verdict == LRSC_UNCONSTRAINED) return;
        if (c->reason == LRSC_REASON_NONE) {
            c->reason = reason;
            c->reason_pc = pc;
        }
        c->verdict = LRSC_UNKNOWN;
    }
}

static lrsc_candidate analyze_lr(lrsc_report *r, int lr_idx)
{
    lrsc_candidate c;
    memset(&c, 0, sizeof(c));
    c.verdict = LRSC_UNKNOWN;
    c.reason = LRSC_REASON_NONE;
    c.lr_pc = r->nodes[lr_idx].dec.pc;

    /* pairing: next SC of matching width, with no intervening LR */
    int sc_idx = -1;
    for (size_t ni = (size_t)lr_idx + 1; ni < r->nnodes; ni++) {
        const rv_decode *d = &r->nodes[ni].dec;
        if (!(r->nodes[ni].flags & LRSC_NODE_REACHABLE)) continue;
        if (is_lr_op(d->op)) {
            set_reason(&c, LRSC_REASON_INTERVENING_LR, d->pc, true);
            break;
        }
        if (is_sc_op(d->op)) {
            if (!same_width(r->nodes[lr_idx].dec.op, d->op)) {
                c.sc_pc = d->pc;
                set_reason(&c, LRSC_REASON_MIXED_WIDTH, d->pc, true);
            } else {
                sc_idx = (int)ni;
                c.sc_pc = d->pc;
            }
            break;
        }
    }

    region_set full = explore_region(r, lr_idx);

    /* status/edge certification first: its success edge bounds the region */
    status_result sr = { LRSC_REASON_NO_STATUS_BRANCH, 0, 0, 0, 0 };
    if (sc_idx >= 0) {
        sr = certify_status(r, &full, lr_idx, sc_idx);
        if (sr.reason != LRSC_REASON_NONE)
            set_reason(&c, sr.reason, sr.reason_pc, false);
    }
    c.branch_pc = sr.branch_pc;
    c.fail_target = sr.fail_target;
    c.success_target = sr.success_target;

    /* the retry region excludes the certified success path */
    region_set s = (sr.reason == LRSC_REASON_NONE)
        ? trim_success_path(r, &full, lr_idx, sr.success_target)
        : full;

    /* pairing-level faults visible on alternate paths inside the region */
    if (c.reason == LRSC_REASON_NONE) {
        size_t lim = s.count < LRSC_MAX_REGION ? s.count : LRSC_MAX_REGION;
        for (size_t i = 0; i < lim; i++) {
            int ni = s.idx[i];
            if (ni == lr_idx || ni == sc_idx) continue;
            const rv_decode *d = &r->nodes[ni].dec;
            if (is_lr_op(d->op)) {
                set_reason(&c, LRSC_REASON_INTERVENING_LR, d->pc, true);
                break;
            }
            if (is_sc_op(d->op)) {
                set_reason(&c, LRSC_REASON_FOREIGN_SC, d->pc, true);
                break;
            }
        }
    }

    /* the success edge must demonstrably leave the region */
    if (sr.reason == LRSC_REASON_NONE) {
        int32_t ok_idx = report_index_at(r, sr.success_target);
        if (ok_idx >= 0 && region_contains(&s, ok_idx))
            set_reason(&c, LRSC_REASON_SUCCESS_EDGE, sr.branch_pc, true);
        else if (ok_idx < 0)
            set_reason(&c, LRSC_REASON_SUCCESS_EDGE, sr.branch_pc, false);
    }

    /* structural conditions over the trimmed region */
    uint64_t why_pc = 0;
    lrsc_reason why = structural_scan(r, &s, lr_idx,
                                      sc_idx >= 0 ? sc_idx : lr_idx, &why_pc);
    if (why != LRSC_REASON_NONE)
        set_reason(&c, why, why_pc, true);

    /* fill the certificate's ordered PC list (capped at the public bound) */
    c.region_count = (uint32_t)(s.count < LRSC_CERT_REGION_MAX
                                ? s.count : LRSC_CERT_REGION_MAX);
    for (size_t i = 0; i < c.region_count; i++)
        c.region_pc[i] = r->nodes[s.idx[i]].dec.pc;

    /* address certification needs a paired SC */
    if (sc_idx >= 0) {
        addr_result ar = certify_address(r, &s, lr_idx, sc_idx);
        if (ar.reason != LRSC_REASON_NONE)
            set_reason(&c, ar.reason, ar.reason_pc, false);
    } else if (c.reason == LRSC_REASON_NONE) {
        c.reason = LRSC_REASON_NO_STATUS_BRANCH;
        c.reason_pc = c.lr_pc;
        c.verdict = LRSC_UNKNOWN;
    }

    if (c.reason == LRSC_REASON_NONE)
        c.verdict = LRSC_PROVEN;
    return c;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

const char *lrsc_verdict_name(lrsc_verdict v)
{
    static const char *names[] = { "proven", "unconstrained", "unknown" };
    return names[v];
}

static const char *const reason_names[] = {
    "none",
    "illegal-instruction",
    "mixed-width",
    "intervening-lr",
    "foreign-sc",
    "forbidden-op",
    "memory-op",
    "fence",
    "system",
    "muldiv",
    "backward-edge",
    "loop",
    "region-too-long",
    "external-entry",
    "edge-outside",
    "address-overwritten",
    "address-merge",
    "address-unknown",
    "status-lost",
    "status-foreign",
    "status-merge",
    "no-status-branch",
    "fail-edge",
    "success-edge",
};

const char *lrsc_reason_name(lrsc_reason reason)
{
    if (reason < 0 || reason >= LRSC_REASON_COUNT) return "invalid";
    return reason_names[reason];
}

const lrsc_node *lrsc_report_nodes(const lrsc_report *r, size_t *count)
{
    if (count) *count = r->nnodes;
    return r->nodes;
}

const lrsc_candidate *lrsc_report_candidates(const lrsc_report *r, size_t *count)
{
    if (count) *count = r->ncand;
    return r->cand;
}

const uint8_t *lrsc_report_code(const lrsc_report *r, size_t *len)
{
    if (len) *len = r->code_len;
    return r->code;
}

uint64_t lrsc_report_base_pc(const lrsc_report *r)
{
    return r->base_pc;
}

size_t lrsc_report_reachable_count(const lrsc_report *r)
{
    return r->reachable;
}

void lrsc_report_free(lrsc_report *r)
{
    if (!r) return;
    free(r->code);
    free(r->nodes);
    free(r->offset_map);
    free(r->npred);
    if (r->preds) {
        for (size_t i = 0; i < r->nnodes; i++) free(r->preds[i]);
        free(r->preds);
    }
    free(r->cand);
    free(r);
}

lrsc_report *lrsc_analyze(rv_isa isa, uint64_t base_pc,
                          const uint8_t *code, size_t len,
                          const uint64_t *entries, size_t nentries)
{
    if (!code || len < 2 || !entries || nentries == 0) return NULL;

    lrsc_report *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->isa = isa;
    r->base_pc = base_pc;
    r->code_len = len;
    r->code = malloc(len);
    if (!r->code) { lrsc_report_free(r); return NULL; }
    memcpy(r->code, code, len);

    if (sweep(r) || build_predecessors(r) ||
        mark_reachable(r, entries, nentries)) {
        lrsc_report_free(r);
        return NULL;
    }

    size_t lrcap = 0;
    for (size_t i = 0; i < r->nnodes; i++) {
        if (!(r->nodes[i].flags & LRSC_NODE_REACHABLE)) continue;
        if (!is_lr_op(r->nodes[i].dec.op)) continue;
        if (r->ncand == lrcap) {
            lrcap = lrcap ? lrcap * 2 : 4;
            lrsc_candidate *grow = realloc(r->cand, lrcap * sizeof(*grow));
            if (!grow) { lrsc_report_free(r); return NULL; }
            r->cand = grow;
        }
        r->cand[r->ncand++] = analyze_lr(r, (int)i);
    }

    return r;
}
