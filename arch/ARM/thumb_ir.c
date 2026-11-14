/* Capstone Disassembly Engine */
/* Thumb IR lifter implementation. See include/capstone/thumb_ir.h. */

#include <stdlib.h>
#include <string.h>

#include <capstone/capstone.h>
#include <capstone/arm.h>
#include <capstone/thumb_ir.h>

/* ------------------------------------------------------------------ */
/* Graph storage                                                       */
/* ------------------------------------------------------------------ */

typedef struct ir_node {
    uint8_t type;    /* cs_thumb_ir_type */
    uint8_t op;      /* cs_thumb_ir_op */
    uint8_t nchild;
    uint8_t pad;
    uint32_t value;  /* CONST value, or entry reg/flag index */
    uint32_t child[3];
} ir_node;

typedef struct ir_insn {
    uint32_t addr;
    uint32_t cond;
    uint32_t nroots;
    cs_thumb_ir_root_ref roots[CS_THUMB_IR_MAX_INSN_ROOTS];
} ir_insn;

struct cs_thumb_ir {
    ir_node *nodes;
    uint32_t node_count;
    uint32_t node_cap;
    ir_insn *insns;
    uint32_t insn_count;
    uint32_t insn_cap;
    uint32_t reg_root[CS_THUMB_IR_REG_COUNT];
    uint32_t flag_root[CS_THUMB_IR_FLAG_COUNT];
    uint32_t mem_root;
    uint32_t const1;   /* CONST (U1) 1: condition of unconditional insns */
};

/* ------------------------------------------------------------------ */
/* Lifter                                                              */
/* ------------------------------------------------------------------ */

typedef struct lift_ctx {
    cs_thumb_ir *g;
    uint32_t it_remaining; /* block instructions still expected */
    uint32_t it_addr;      /* address of the open IT instruction */
    cs_thumb_ir_err err;
    uint32_t err_addr;
} lift_ctx;

static cs_thumb_ir_err lift_fail(lift_ctx *lc, cs_thumb_ir_err err, uint32_t addr)
{
    if (lc->err == CS_THUMB_IR_OK) {
        lc->err = err;
        lc->err_addr = addr;
    }
    return err;
}

/* Append a node; children must already exist (keeps topological order). */
static uint32_t mk_node(lift_ctx *lc, uint32_t insn_addr, uint8_t type, uint8_t op,
                        uint32_t value, uint32_t c0, uint32_t c1, uint32_t c2,
                        uint8_t nchild)
{
    cs_thumb_ir *g = lc->g;
    ir_node *n;
    if (g->node_count == g->node_cap) {
        uint32_t ncap = g->node_cap ? g->node_cap * 2 : 64;
        ir_node *nn;
        if (ncap < g->node_cap || ncap > 0x10000000u) {
            lift_fail(lc, CS_THUMB_IR_ERR_NOMEM, insn_addr);
            return CS_THUMB_IR_INVALID_NODE;
        }
        nn = (ir_node *)realloc(g->nodes, (size_t)ncap * sizeof(ir_node));
        if (!nn) {
            lift_fail(lc, CS_THUMB_IR_ERR_NOMEM, insn_addr);
            return CS_THUMB_IR_INVALID_NODE;
        }
        g->nodes = nn;
        g->node_cap = ncap;
    }
    n = &g->nodes[g->node_count];
    n->type = type;
    n->op = op;
    n->nchild = nchild;
    n->pad = 0;
    n->value = value;
    n->child[0] = c0;
    n->child[1] = c1;
    n->child[2] = c2;
    return g->node_count++;
}

#define MK0(lc, a, t, o, v) mk_node((lc), (a), (t), (o), (v), 0, 0, 0, 0)
#define MK1(lc, a, t, o, x) mk_node((lc), (a), (t), (o), 0, (x), 0, 0, 1)
#define MK2(lc, a, t, o, x, y) mk_node((lc), (a), (t), (o), 0, (x), (y), 0, 2)
#define MK3(lc, a, t, o, x, y, z) mk_node((lc), (a), (t), (o), 0, (x), (y), (z), 3)

static uint32_t mk_const32(lift_ctx *lc, uint32_t a, uint32_t v)
{
    return MK0(lc, a, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_CONST, v);
}

/* r0..r12 only; returns register index or -1. */
static int reg_index(int reg)
{
    if (reg >= ARM_REG_R0 && reg <= ARM_REG_R0 + (CS_THUMB_IR_REG_COUNT - 1))
        return reg - ARM_REG_R0;
    return -1;
}

static bool reg_op_ok(const cs_arm_op *op)
{
    return op->type == ARM_OP_REG && op->shift.type == ARM_SFT_INVALID &&
           reg_index(op->reg) >= 0;
}

/* Execution condition for cc (EQ..LE) over the *current* flag roots. */
static uint32_t build_cond(lift_ctx *lc, uint32_t addr, arm_cc cc)
{
    cs_thumb_ir *g = lc->g;
    uint32_t N = g->flag_root[CS_THUMB_IR_FLAG_N];
    uint32_t Z = g->flag_root[CS_THUMB_IR_FLAG_Z];
    uint32_t C = g->flag_root[CS_THUMB_IR_FLAG_C];
    uint32_t V = g->flag_root[CS_THUMB_IR_FLAG_V];
    uint32_t t;
    switch (cc) {
    case ARM_CC_EQ: return Z;
    case ARM_CC_NE: return MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, Z);
    case ARM_CC_HS: return C;
    case ARM_CC_LO: return MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, C);
    case ARM_CC_MI: return N;
    case ARM_CC_PL: return MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, N);
    case ARM_CC_VS: return V;
    case ARM_CC_VC: return MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, V);
    case ARM_CC_HI:
        t = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, Z);
        return MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_AND, C, t);
    case ARM_CC_LS:
        t = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, C);
        return MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_OR, t, Z);
    case ARM_CC_GE:
        t = MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_XOR, N, V);
        return MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, t);
    case ARM_CC_LT:
        return MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_XOR, N, V);
    case ARM_CC_GT:
        t = MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_XOR, N, V);
        t = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, t);
        t = MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_AND, t,
                MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_NOT, Z));
        return t;
    case ARM_CC_LE:
        t = MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_XOR, N, V);
        return MK2(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_OR, Z, t);
    default:
        lift_fail(lc, CS_THUMB_IR_ERR_UNSUPPORTED, addr);
        return CS_THUMB_IR_INVALID_NODE;
    }
}

/* Little-endian word load: four consecutive bytes, address wraps uint32. */
static uint32_t build_load32(lift_ctx *lc, uint32_t addr, uint32_t mem, uint32_t addr_node)
{
    uint32_t b[4], a, w01, w23;
    int i;
    for (i = 0; i < 4; i++) {
        a = addr_node;
        if (i) {
            uint32_t off = mk_const32(lc, addr, (uint32_t)i);
            a = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_ADD, addr_node, off);
        }
        b[i] = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_LOAD8, mem, a);
    }
    for (i = 1; i < 4; i++) {
        uint32_t sh = mk_const32(lc, addr, (uint32_t)(8 * i));
        b[i] = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_SHL, b[i], sh);
    }
    w01 = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_OR, b[0], b[1]);
    w23 = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_OR, b[2], b[3]);
    return MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_OR, w01, w23);
}

/* Little-endian word store: four consecutive byte stores. */
static uint32_t build_store32(lift_ctx *lc, uint32_t addr, uint32_t mem,
                              uint32_t addr_node, uint32_t val)
{
    uint32_t m = mem;
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t a = addr_node, v = val;
        if (i) {
            uint32_t off = mk_const32(lc, addr, (uint32_t)i);
            uint32_t sh = mk_const32(lc, addr, (uint32_t)(8 * i));
            a = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_ADD, addr_node, off);
            v = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_SHR, val, sh);
        }
        m = MK3(lc, addr, CS_THUMB_IR_TYPE_MEM, CS_THUMB_IR_OP_STORE8, m, a, v);
    }
    return m;
}

/* Pending result-root writes of one instruction. */
typedef struct ir_write {
    uint32_t kind;   /* cs_thumb_ir_root_kind */
    uint32_t index;
    uint32_t node;
} ir_write;

static uint32_t old_root(const cs_thumb_ir *g, uint32_t kind, uint32_t index)
{
    switch (kind) {
    case CS_THUMB_IR_ROOT_REG: return g->reg_root[index];
    case CS_THUMB_IR_ROOT_FLAG: return g->flag_root[index];
    default: return g->mem_root;
    }
}

static void set_root(cs_thumb_ir *g, uint32_t kind, uint32_t index, uint32_t node)
{
    switch (kind) {
    case CS_THUMB_IR_ROOT_REG: g->reg_root[index] = node; break;
    case CS_THUMB_IR_ROOT_FLAG: g->flag_root[index] = node; break;
    default: g->mem_root = node; break;
    }
}

/* Operand as a value node: low register or immediate. */
static uint32_t operand_value(lift_ctx *lc, uint32_t addr, const cs_arm_op *op, bool *ok)
{
    if (op->type == ARM_OP_REG && op->shift.type == ARM_SFT_INVALID &&
        reg_index(op->reg) >= 0) {
        *ok = true;
        return lc->g->reg_root[reg_index(op->reg)];
    }
    if (op->type == ARM_OP_IMM) {
        *ok = true;
        return mk_const32(lc, addr, (uint32_t)op->imm);
    }
    *ok = false;
    return CS_THUMB_IR_INVALID_NODE;
}

/* Flag roots after a 32-bit add/sub of a and b producing res. */
static void write_arith_flags(lift_ctx *lc, uint32_t addr, bool is_sub,
                              uint32_t a, uint32_t b, uint32_t res,
                              ir_write *w, int *nw)
{
    uint32_t n = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_MSB, res);
    uint32_t z = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_IS_ZERO, res);
    uint32_t c = MK2(lc, addr, CS_THUMB_IR_TYPE_U1,
                     is_sub ? CS_THUMB_IR_OP_CARRY_SUB : CS_THUMB_IR_OP_CARRY_ADD, a, b);
    uint32_t v = MK2(lc, addr, CS_THUMB_IR_TYPE_U1,
                     is_sub ? CS_THUMB_IR_OP_OVF_SUB : CS_THUMB_IR_OP_OVF_ADD, a, b);
    w[(*nw)++] = (ir_write){ CS_THUMB_IR_ROOT_FLAG, CS_THUMB_IR_FLAG_N, n };
    w[(*nw)++] = (ir_write){ CS_THUMB_IR_ROOT_FLAG, CS_THUMB_IR_FLAG_Z, z };
    w[(*nw)++] = (ir_write){ CS_THUMB_IR_ROOT_FLAG, CS_THUMB_IR_FLAG_C, c };
    w[(*nw)++] = (ir_write){ CS_THUMB_IR_ROOT_FLAG, CS_THUMB_IR_FLAG_V, v };
}

/* MOVS updates only N/Z; C/V keep their previous version. */
static void write_mov_flags(lift_ctx *lc, uint32_t addr, uint32_t res,
                            ir_write *w, int *nw)
{
    uint32_t n = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_MSB, res);
    uint32_t z = MK1(lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_IS_ZERO, res);
    w[(*nw)++] = (ir_write){ CS_THUMB_IR_ROOT_FLAG, CS_THUMB_IR_FLAG_N, n };
    w[(*nw)++] = (ir_write){ CS_THUMB_IR_ROOT_FLAG, CS_THUMB_IR_FLAG_Z, z };
}

static bool mem_operand_ok(const cs_arm *arm, const cs_arm_op *op)
{
    return op->type == ARM_OP_MEM && !arm->writeback && !op->subtracted &&
           op->mem.index == ARM_REG_INVALID && op->mem.disp >= 0 &&
           reg_index(op->mem.base) >= 0;
}

static uint32_t mem_addr_node(lift_ctx *lc, uint32_t addr, const cs_arm_op *op)
{
    uint32_t base = lc->g->reg_root[reg_index(op->mem.base)];
    if (op->mem.disp == 0)
        return base;
    return MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_ADD, base,
               mk_const32(lc, addr, (uint32_t)op->mem.disp));
}

/* Lift one decoded instruction. Returns false on error (lc->err set). */
static bool lift_one(lift_ctx *lc, const cs_insn *insn)
{
    cs_thumb_ir *g = lc->g;
    const cs_arm *arm = &insn->detail->arm;
    uint32_t addr = (uint32_t)insn->address;
    ir_write writes[CS_THUMB_IR_MAX_INSN_ROOTS];
    int nw = 0;
    uint32_t cond = g->const1;
    bool predicated;
    ir_insn *rec;
    int i;

    if (insn->id == ARM_INS_IT) {
        unsigned mask, tz, len;
        if (lc->it_remaining) {
            lift_fail(lc, CS_THUMB_IR_ERR_IT, addr); /* nested IT */
            return false;
        }
        if (arm->cc < ARM_CC_EQ || arm->cc > ARM_CC_LE) {
            lift_fail(lc, CS_THUMB_IR_ERR_UNSUPPORTED, addr);
            return false;
        }
        /* IT executes unconditionally; the block length comes from the
           mask bits of its own encoding (0xBF<firstcond><mask>). */
        mask = insn->bytes[0] & 0xFu;
        tz = 0;
        while (tz < 4 && !(mask & (1u << tz)))
            tz++;
        if (tz == 4) {
            lift_fail(lc, CS_THUMB_IR_ERR_UNSUPPORTED, addr);
            return false;
        }
        len = 4 - tz; /* 1..4 instructions */
        lc->it_remaining = len;
        lc->it_addr = addr;
        /* record the IT itself: unconditional, modifies nothing */
        if (g->insn_count == g->insn_cap) {
            uint32_t ncap = g->insn_cap ? g->insn_cap * 2 : 16;
            ir_insn *ni = (ir_insn *)realloc(g->insns, (size_t)ncap * sizeof(ir_insn));
            if (!ni) {
                lift_fail(lc, CS_THUMB_IR_ERR_NOMEM, addr);
                return false;
            }
            g->insns = ni;
            g->insn_cap = ncap;
        }
        rec = &g->insns[g->insn_count++];
        rec->addr = addr;
        rec->cond = g->const1;
        rec->nroots = 0;
        return true;
    }

    predicated = lc->it_remaining > 0;
    if (arm->cc != ARM_CC_AL) {
        cond = build_cond(lc, addr, arm->cc);
        if (cond == CS_THUMB_IR_INVALID_NODE)
            return false;
    }

    switch (insn->id) {
    case ARM_INS_MOV:
    case ARM_INS_MOVS: {
        const cs_arm_op *d, *s;
        uint32_t val;
        bool ok;
        if (insn->size == 4) {
            /* 32-bit: only MOVW (11110i100100....); MOV.W/MOVS.W and any
               other wide encoding are rejected. */
            uint32_t hw1 = (uint32_t)insn->bytes[0] | ((uint32_t)insn->bytes[1] << 8);
            if ((hw1 & 0xFBF0u) != 0xF240u || arm->op_count != 2)
                break;
            d = &arm->operands[0];
            s = &arm->operands[1];
            if (d->type != ARM_OP_REG || reg_index(d->reg) < 0 ||
                s->type != ARM_OP_IMM)
                break;
            val = mk_const32(lc, addr, (uint32_t)s->imm & 0xFFFFu);
            writes[nw++] = (ir_write){ CS_THUMB_IR_ROOT_REG, (uint32_t)reg_index(d->reg), val };
            goto commit;
        }
        if (insn->size != 2 || arm->op_count != 2)
            break;
        d = &arm->operands[0];
        s = &arm->operands[1];
        if (!reg_op_ok(d))
            break;
        val = operand_value(lc, addr, s, &ok);
        if (!ok)
            break;
        writes[nw++] = (ir_write){ CS_THUMB_IR_ROOT_REG, (uint32_t)reg_index(d->reg), val };
        if (arm->update_flags)
            write_mov_flags(lc, addr, val, writes, &nw);
        goto commit;
    }
    case ARM_INS_MOVT: {
        const cs_arm_op *d, *s;
        uint32_t old, lo, hi, val;
        if (insn->size != 4 || arm->op_count != 2)
            break;
        d = &arm->operands[0];
        s = &arm->operands[1];
        if (d->type != ARM_OP_REG || reg_index(d->reg) < 0 ||
            s->type != ARM_OP_IMM)
            break;
        old = g->reg_root[reg_index(d->reg)];
        lo = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_AND, old,
                 mk_const32(lc, addr, 0xFFFFu));
        hi = mk_const32(lc, addr, ((uint32_t)s->imm & 0xFFFFu) << 16);
        val = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_OR, lo, hi);
        writes[nw++] = (ir_write){ CS_THUMB_IR_ROOT_REG, (uint32_t)reg_index(d->reg), val };
        goto commit;
    }
    case ARM_INS_ADD:
    case ARM_INS_SUB: {
        const cs_arm_op *d, *a, *b;
        uint32_t av, bv, res;
        bool ok, is_sub = insn->id == ARM_INS_SUB;
        if (insn->size != 2)
            break;
        if (arm->op_count == 3) {
            d = &arm->operands[0];
            a = &arm->operands[1];
            b = &arm->operands[2];
        } else if (arm->op_count == 2) {
            d = &arm->operands[0];
            a = &arm->operands[0];
            b = &arm->operands[1];
        } else {
            break;
        }
        if (!reg_op_ok(d) || !reg_op_ok(a))
            break;
        av = g->reg_root[reg_index(a->reg)];
        bv = operand_value(lc, addr, b, &ok);
        if (!ok)
            break;
        res = MK2(lc, addr, CS_THUMB_IR_TYPE_U32,
                  is_sub ? CS_THUMB_IR_OP_SUB : CS_THUMB_IR_OP_ADD, av, bv);
        writes[nw++] = (ir_write){ CS_THUMB_IR_ROOT_REG, (uint32_t)reg_index(d->reg), res };
        if (arm->update_flags)
            write_arith_flags(lc, addr, is_sub, av, bv, res, writes, &nw);
        goto commit;
    }
    case ARM_INS_CMP: {
        const cs_arm_op *a, *b;
        uint32_t av, bv, res;
        bool ok;
        if (insn->size != 2 || arm->op_count != 2)
            break;
        a = &arm->operands[0];
        b = &arm->operands[1];
        if (!reg_op_ok(a))
            break;
        av = g->reg_root[reg_index(a->reg)];
        bv = operand_value(lc, addr, b, &ok);
        if (!ok)
            break;
        res = MK2(lc, addr, CS_THUMB_IR_TYPE_U32, CS_THUMB_IR_OP_SUB, av, bv);
        /* CMP always updates the flags (also inside an IT block). */
        write_arith_flags(lc, addr, true, av, bv, res, writes, &nw);
        goto commit;
    }
    case ARM_INS_LDR: {
        const cs_arm_op *d, *m;
        uint32_t an, val;
        if (insn->size != 2 || arm->op_count != 2)
            break;
        d = &arm->operands[0];
        m = &arm->operands[1];
        if (!reg_op_ok(d) || !mem_operand_ok(arm, m))
            break;
        an = mem_addr_node(lc, addr, m);
        val = build_load32(lc, addr, g->mem_root, an);
        writes[nw++] = (ir_write){ CS_THUMB_IR_ROOT_REG, (uint32_t)reg_index(d->reg), val };
        goto commit;
    }
    case ARM_INS_STR: {
        const cs_arm_op *s, *m;
        uint32_t an, nm;
        if (insn->size != 2 || arm->op_count != 2)
            break;
        s = &arm->operands[0];
        m = &arm->operands[1];
        if (!reg_op_ok(s) || !mem_operand_ok(arm, m))
            break;
        an = mem_addr_node(lc, addr, m);
        nm = build_store32(lc, addr, g->mem_root, an, g->reg_root[reg_index(s->reg)]);
        writes[nw++] = (ir_write){ CS_THUMB_IR_ROOT_MEM, 0, nm };
        goto commit;
    }
    case ARM_INS_NOP:
        goto commit;
    case ARM_INS_HINT:
        /* Capstone reports the Thumb hint space as ARM_INS_HINT; only the
           NOP encoding (0xBF00) is supported, other hints are rejected. */
        if (insn->size == 2 && insn->bytes[0] == 0x00 && insn->bytes[1] == 0xBF)
            goto commit;
        break;
    default:
        break;
    }

    /* unsupported instruction or operand shape */
    lift_fail(lc, CS_THUMB_IR_ERR_UNSUPPORTED, addr);
    return false;

commit:
    if (lc->err != CS_THUMB_IR_OK)
        return false;
    /* A predicated instruction keeps the old root version when its
       condition is false: wrap every write in a version select. */
    if (cond != g->const1) {
        for (i = 0; i < nw; i++) {
            uint32_t prev = old_root(g, writes[i].kind, writes[i].index);
            uint32_t type = (writes[i].kind == CS_THUMB_IR_ROOT_MEM)
                                ? CS_THUMB_IR_TYPE_MEM
                                : (writes[i].kind == CS_THUMB_IR_ROOT_FLAG
                                       ? CS_THUMB_IR_TYPE_U1
                                       : CS_THUMB_IR_TYPE_U32);
            writes[i].node = MK3(lc, addr, type, CS_THUMB_IR_OP_SELECT,
                                 cond, writes[i].node, prev);
        }
        if (lc->err != CS_THUMB_IR_OK)
            return false;
    }
    for (i = 0; i < nw; i++)
        set_root(g, writes[i].kind, writes[i].index, writes[i].node);
    /* record the instruction */
    if (g->insn_count == g->insn_cap) {
        uint32_t ncap = g->insn_cap ? g->insn_cap * 2 : 16;
        ir_insn *ni = (ir_insn *)realloc(g->insns, (size_t)ncap * sizeof(ir_insn));
        if (!ni) {
            lift_fail(lc, CS_THUMB_IR_ERR_NOMEM, addr);
            return false;
        }
        g->insns = ni;
        g->insn_cap = ncap;
    }
    rec = &g->insns[g->insn_count++];
    rec->addr = addr;
    rec->cond = cond;
    rec->nroots = (uint32_t)nw;
    for (i = 0; i < nw; i++) {
        rec->roots[i].kind = writes[i].kind;
        rec->roots[i].index = writes[i].index;
        rec->roots[i].node = writes[i].node;
    }
    if (predicated)
        lc->it_remaining--;
    return true;
}

static void graph_free_members(cs_thumb_ir *g)
{
    if (!g)
        return;
    free(g->nodes);
    free(g->insns);
    free(g);
}

cs_thumb_ir_err CAPSTONE_API cs_thumb_ir_lift(const uint8_t *code, size_t size,
                                              uint32_t addr,
                                              cs_thumb_ir **out,
                                              cs_thumb_ir_error *err)
{
    cs_thumb_ir *g = NULL;
    lift_ctx lc;
    csh h = 0;
    cs_insn *insn = NULL;
    cs_thumb_ir_err res = CS_THUMB_IR_OK;
    uint32_t err_addr = addr;
    int i;

    if (err) {
        err->code = CS_THUMB_IR_OK;
        err->addr = addr;
    }
    if (!out)
        return CS_THUMB_IR_ERR_ARG;
    *out = NULL;
    if (!code && size) {
        res = CS_THUMB_IR_ERR_ARG;
        goto fail;
    }
    if (size && (uint64_t)addr + (uint64_t)(size - 1) > 0xFFFFFFFFull) {
        res = CS_THUMB_IR_ERR_RANGE;
        goto fail;
    }

    g = (cs_thumb_ir *)calloc(1, sizeof(cs_thumb_ir));
    if (!g) {
        res = CS_THUMB_IR_ERR_NOMEM;
        goto fail;
    }
    memset(&lc, 0, sizeof(lc));
    lc.g = g;
    lc.err = CS_THUMB_IR_OK;

    /* entry state: r0..r12, N/Z/C/V, entry memory, and the shared
       unconditional-execution condition constant */
    for (i = 0; i < CS_THUMB_IR_REG_COUNT; i++) {
        g->reg_root[i] = MK0(&lc, addr, CS_THUMB_IR_TYPE_U32,
                             CS_THUMB_IR_OP_INPUT_REG, (uint32_t)i);
    }
    for (i = 0; i < CS_THUMB_IR_FLAG_COUNT; i++) {
        g->flag_root[i] = MK0(&lc, addr, CS_THUMB_IR_TYPE_U1,
                              CS_THUMB_IR_OP_INPUT_FLAG, (uint32_t)i);
    }
    g->mem_root = MK0(&lc, addr, CS_THUMB_IR_TYPE_MEM, CS_THUMB_IR_OP_INPUT_MEM, 0);
    g->const1 = MK0(&lc, addr, CS_THUMB_IR_TYPE_U1, CS_THUMB_IR_OP_CONST, 1);
    if (lc.err != CS_THUMB_IR_OK) {
        res = lc.err;
        err_addr = lc.err_addr;
        goto fail;
    }

    if (size) {
        const uint8_t *p;
        size_t remain;
        uint64_t pc;

        if (cs_open(CS_ARCH_ARM, CS_MODE_THUMB, &h) != CS_ERR_OK) {
            res = CS_THUMB_IR_ERR_NOMEM;
            goto fail;
        }
        cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
        cs_option(h, CS_OPT_SKIPDATA, CS_OPT_OFF);
        insn = cs_malloc(h);
        if (!insn) {
            res = CS_THUMB_IR_ERR_NOMEM;
            goto fail;
        }
        p = code;
        remain = size;
        pc = addr;
        while (remain) {
            if (!cs_disasm_iter(h, &p, &remain, &pc, insn)) {
                res = CS_THUMB_IR_ERR_DECODE;
                err_addr = (uint32_t)pc;
                goto fail;
            }
            if (!lift_one(&lc, insn)) {
                res = lc.err;
                err_addr = lc.err_addr;
                goto fail;
            }
        }
        if (lc.it_remaining) {
            /* the fragment ends before the IT block is complete */
            res = CS_THUMB_IR_ERR_IT;
            err_addr = lc.it_addr;
            goto fail;
        }
    }

    if (insn)
        cs_free(insn, 1);
    if (h)
        cs_close(&h);
    *out = g;
    return CS_THUMB_IR_OK;

fail:
    if (insn)
        cs_free(insn, 1);
    if (h)
        cs_close(&h);
    graph_free_members(g);
    if (err) {
        err->code = res;
        err->addr = err_addr;
    }
    return res;
}

void CAPSTONE_API cs_thumb_ir_free(cs_thumb_ir *graph)
{
    graph_free_members(graph);
}

const char *CAPSTONE_API cs_thumb_ir_strerror(cs_thumb_ir_err err)
{
    switch (err) {
    case CS_THUMB_IR_OK: return "no error";
    case CS_THUMB_IR_ERR_ARG: return "invalid argument";
    case CS_THUMB_IR_ERR_DECODE: return "illegal or truncated encoding";
    case CS_THUMB_IR_ERR_UNSUPPORTED: return "unsupported instruction or operand";
    case CS_THUMB_IR_ERR_IT: return "nested or unterminated IT block";
    case CS_THUMB_IR_ERR_RANGE: return "address range wraps around";
    case CS_THUMB_IR_ERR_NOMEM: return "out of memory";
    case CS_THUMB_IR_ERR_READ: return "entry memory read failed";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

uint32_t CAPSTONE_API cs_thumb_ir_node_count(const cs_thumb_ir *graph)
{
    return graph ? graph->node_count : 0;
}

bool CAPSTONE_API cs_thumb_ir_get_node(const cs_thumb_ir *graph, uint32_t id,
                                        cs_thumb_ir_node_info *info)
{
    const ir_node *n;
    int i;
    if (!graph || !info || id >= graph->node_count)
        return false;
    n = &graph->nodes[id];
    info->type = n->type;
    info->op = n->op;
    info->value = n->value;
    info->child_count = n->nchild;
    for (i = 0; i < 3; i++)
        info->child[i] = (i < n->nchild) ? n->child[i] : CS_THUMB_IR_INVALID_NODE;
    return true;
}

uint32_t CAPSTONE_API cs_thumb_ir_root_reg(const cs_thumb_ir *graph, uint32_t reg)
{
    if (!graph || reg >= CS_THUMB_IR_REG_COUNT)
        return CS_THUMB_IR_INVALID_NODE;
    return graph->reg_root[reg];
}

uint32_t CAPSTONE_API cs_thumb_ir_root_flag(const cs_thumb_ir *graph, uint32_t flag)
{
    if (!graph || flag >= CS_THUMB_IR_FLAG_COUNT)
        return CS_THUMB_IR_INVALID_NODE;
    return graph->flag_root[flag];
}

uint32_t CAPSTONE_API cs_thumb_ir_root_mem(const cs_thumb_ir *graph)
{
    if (!graph)
        return CS_THUMB_IR_INVALID_NODE;
    return graph->mem_root;
}

uint32_t CAPSTONE_API cs_thumb_ir_insn_count(const cs_thumb_ir *graph)
{
    return graph ? graph->insn_count : 0;
}

bool CAPSTONE_API cs_thumb_ir_get_insn(const cs_thumb_ir *graph, uint32_t idx,
                                        cs_thumb_ir_insn_info *info)
{
    const ir_insn *r;
    uint32_t i;
    if (!graph || !info || idx >= graph->insn_count)
        return false;
    r = &graph->insns[idx];
    info->addr = r->addr;
    info->cond = r->cond;
    info->root_count = r->nroots;
    for (i = 0; i < CS_THUMB_IR_MAX_INSN_ROOTS; i++) {
        if (i < r->nroots) {
            info->roots[i] = r->roots[i];
        } else {
            info->roots[i].kind = 0;
            info->roots[i].index = 0;
            info->roots[i].node = CS_THUMB_IR_INVALID_NODE;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                          */
/* ------------------------------------------------------------------ */

typedef struct eval_ctx {
    const cs_thumb_ir *g;
    const cs_thumb_ir_inputs *in;
    uint8_t *state;    /* per node: 0 = unevaluated, 1 = value cached */
    uint32_t *vals;    /* cached scalar values */
    uint32_t fail_addr;
    bool failed;
} eval_ctx;

static bool eval_scalar(eval_ctx *e, uint32_t id, uint32_t *out);
static bool eval_mem_byte(eval_ctx *e, uint32_t mem_id, uint32_t addr, uint8_t *out);

static bool eval_fail(eval_ctx *e, uint32_t addr)
{
    e->failed = true;
    e->fail_addr = addr;
    return false;
}

static bool eval_mem_byte(eval_ctx *e, uint32_t mem_id, uint32_t addr, uint8_t *out)
{
    const ir_node *n = &e->g->nodes[mem_id];
    uint32_t a, c;
    switch (n->op) {
    case CS_THUMB_IR_OP_INPUT_MEM:
        if (!e->in->read8 || !e->in->read8(e->in->read8_ud, addr, out))
            return eval_fail(e, addr);
        return true;
    case CS_THUMB_IR_OP_STORE8:
        if (!eval_scalar(e, n->child[1], &a))
            return false;
        if (a == addr) {
            uint32_t v;
            if (!eval_scalar(e, n->child[2], &v))
                return false;
            *out = (uint8_t)(v & 0xFFu);
            return true;
        }
        return eval_mem_byte(e, n->child[0], addr, out);
    case CS_THUMB_IR_OP_SELECT:
        /* lazy: only the taken memory version is inspected */
        if (!eval_scalar(e, n->child[0], &c))
            return false;
        return eval_mem_byte(e, n->child[(c & 1u) ? 1 : 2], addr, out);
    default:
        return false;
    }
}

static bool eval_scalar(eval_ctx *e, uint32_t id, uint32_t *out)
{
    const cs_thumb_ir *g = e->g;
    const ir_node *n = &g->nodes[id];
    uint32_t a, b, v;
    if (e->state[id]) {
        *out = e->vals[id];
        return true;
    }
    switch (n->op) {
    case CS_THUMB_IR_OP_CONST:
        v = n->value;
        break;
    case CS_THUMB_IR_OP_INPUT_REG:
        v = e->in->regs[n->value];
        break;
    case CS_THUMB_IR_OP_INPUT_FLAG:
        v = e->in->flags[n->value] ? 1u : 0u;
        break;
    case CS_THUMB_IR_OP_ADD:
    case CS_THUMB_IR_OP_SUB:
    case CS_THUMB_IR_OP_AND:
    case CS_THUMB_IR_OP_OR:
    case CS_THUMB_IR_OP_XOR:
    case CS_THUMB_IR_OP_SHL:
    case CS_THUMB_IR_OP_SHR:
    case CS_THUMB_IR_OP_CARRY_ADD:
    case CS_THUMB_IR_OP_CARRY_SUB:
    case CS_THUMB_IR_OP_OVF_ADD:
    case CS_THUMB_IR_OP_OVF_SUB:
        if (!eval_scalar(e, n->child[0], &a) || !eval_scalar(e, n->child[1], &b))
            return false;
        switch (n->op) {
        case CS_THUMB_IR_OP_ADD: v = a + b; break;
        case CS_THUMB_IR_OP_SUB: v = a - b; break;
        case CS_THUMB_IR_OP_AND: v = a & b; break;
        case CS_THUMB_IR_OP_OR: v = a | b; break;
        case CS_THUMB_IR_OP_XOR: v = a ^ b; break;
        case CS_THUMB_IR_OP_SHL: v = a << (b & 31u); break;
        case CS_THUMB_IR_OP_SHR: v = a >> (b & 31u); break;
        case CS_THUMB_IR_OP_CARRY_ADD: v = (uint32_t)(((uint64_t)a + b) >> 32); break;
        case CS_THUMB_IR_OP_CARRY_SUB: v = (a >= b) ? 1u : 0u; break;
        case CS_THUMB_IR_OP_OVF_ADD: {
            uint32_t r = a + b;
            v = (~(a ^ b) & (a ^ r)) >> 31;
            break;
        }
        default: { /* CS_THUMB_IR_OP_OVF_SUB */
            uint32_t r = a - b;
            v = ((a ^ b) & (a ^ r)) >> 31;
            break;
        }
        }
        break;
    case CS_THUMB_IR_OP_NOT:
    case CS_THUMB_IR_OP_IS_ZERO:
    case CS_THUMB_IR_OP_MSB:
        if (!eval_scalar(e, n->child[0], &a))
            return false;
        if (n->op == CS_THUMB_IR_OP_NOT)
            v = (n->type == CS_THUMB_IR_TYPE_U1) ? !a : ~a;
        else if (n->op == CS_THUMB_IR_OP_IS_ZERO)
            v = (a == 0) ? 1u : 0u;
        else
            v = a >> 31;
        break;
    case CS_THUMB_IR_OP_SELECT:
        /* lazy: the branch not taken is never evaluated */
        if (!eval_scalar(e, n->child[0], &a))
            return false;
        if (!eval_scalar(e, n->child[(a & 1u) ? 1 : 2], &v))
            return false;
        break;
    case CS_THUMB_IR_OP_LOAD8: {
        uint8_t byte;
        if (!eval_scalar(e, n->child[1], &a))
            return false;
        if (!eval_mem_byte(e, n->child[0], a, &byte))
            return false;
        v = byte;
        break;
    }
    default:
        return false;
    }
    e->state[id] = 1;
    e->vals[id] = v;
    *out = v;
    return true;
}

static cs_thumb_ir_err eval_prepare(const cs_thumb_ir *graph,
                                    const cs_thumb_ir_inputs *in,
                                    eval_ctx *e)
{
    if (!graph || !in)
        return CS_THUMB_IR_ERR_ARG;
    memset(e, 0, sizeof(*e));
    e->g = graph;
    e->in = in;
    e->state = (uint8_t *)calloc(graph->node_count, 1);
    e->vals = (uint32_t *)malloc((size_t)graph->node_count * sizeof(uint32_t));
    if (!e->state || !e->vals) {
        free(e->state);
        free(e->vals);
        return CS_THUMB_IR_ERR_NOMEM;
    }
    return CS_THUMB_IR_OK;
}

static void eval_done(eval_ctx *e)
{
    free(e->state);
    free(e->vals);
}

cs_thumb_ir_err CAPSTONE_API cs_thumb_ir_eval_node(const cs_thumb_ir *graph,
                                                   const cs_thumb_ir_inputs *in,
                                                   uint32_t node,
                                                   uint32_t *out,
                                                   uint32_t *fail_addr)
{
    eval_ctx e;
    cs_thumb_ir_err err;
    if (fail_addr)
        *fail_addr = 0;
    if (!graph || !out || node >= graph->node_count ||
        graph->nodes[node].type == CS_THUMB_IR_TYPE_MEM)
        return CS_THUMB_IR_ERR_ARG;
    err = eval_prepare(graph, in, &e);
    if (err != CS_THUMB_IR_OK)
        return err;
    if (!eval_scalar(&e, node, out)) {
        if (fail_addr)
            *fail_addr = e.fail_addr;
        eval_done(&e);
        return CS_THUMB_IR_ERR_READ;
    }
    eval_done(&e);
    return CS_THUMB_IR_OK;
}

cs_thumb_ir_err CAPSTONE_API cs_thumb_ir_eval_mem8(const cs_thumb_ir *graph,
                                                   const cs_thumb_ir_inputs *in,
                                                   uint32_t addr,
                                                   uint8_t *out,
                                                   uint32_t *fail_addr)
{
    eval_ctx e;
    cs_thumb_ir_err err;
    if (fail_addr)
        *fail_addr = 0;
    if (!graph || !out)
        return CS_THUMB_IR_ERR_ARG;
    err = eval_prepare(graph, in, &e);
    if (err != CS_THUMB_IR_OK)
        return err;
    if (!eval_mem_byte(&e, graph->mem_root, addr, out)) {
        if (fail_addr)
            *fail_addr = e.fail_addr;
        eval_done(&e);
        return CS_THUMB_IR_ERR_READ;
    }
    eval_done(&e);
    return CS_THUMB_IR_OK;
}
