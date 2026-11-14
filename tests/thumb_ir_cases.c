/* Task cases for the Thumb IR lifter: groups flags / predication / memory.
   All expected values below are hand-computed from the ARM semantics,
   not derived from the new interface itself. */

#include <string.h>

#include <capstone/capstone.h>
#include <capstone/thumb_ir.h>

#include "task_support/check.h"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static cs_thumb_ir *lift_ok(const uint8_t *code, size_t n, uint32_t addr)
{
    cs_thumb_ir *g = NULL;
    if (cs_thumb_ir_lift(code, n, addr, &g, NULL) != CS_THUMB_IR_OK)
        return NULL;
    return g;
}

static void inputs_zero(cs_thumb_ir_inputs *in)
{
    memset(in, 0, sizeof(*in));
}

static int get_node(const cs_thumb_ir *g, const cs_thumb_ir_inputs *in,
                    uint32_t node, uint32_t *out)
{
    return cs_thumb_ir_eval_node(g, in, node, out, NULL) == CS_THUMB_IR_OK;
}

static int get_reg(const cs_thumb_ir *g, const cs_thumb_ir_inputs *in,
                   uint32_t reg, uint32_t *out)
{
    return get_node(g, in, cs_thumb_ir_root_reg(g, reg), out);
}

static int get_flag(const cs_thumb_ir *g, const cs_thumb_ir_inputs *in,
                    uint32_t flag, uint32_t *out)
{
    return get_node(g, in, cs_thumb_ir_root_flag(g, flag), out);
}

/* entry-memory read callback model */
struct memcb {
    unsigned calls;
    uint32_t addrs[16];
    uint32_t fault_lo, fault_hi; /* inclusive faulting range */
    int use_pattern;             /* 1: byte = addr & 0xff, 0: fixed */
    uint8_t fixed;
    int fail_all;
};

static void memcb_init(struct memcb *m)
{
    memset(m, 0, sizeof(*m));
    m->fault_lo = 1; /* empty range by default */
    m->fault_hi = 0;
}

static bool mem_read8(void *ud, uint32_t addr, uint8_t *out)
{
    struct memcb *m = (struct memcb *)ud;
    if (m->calls < 16)
        m->addrs[m->calls] = addr;
    m->calls++;
    if (m->fail_all)
        return false;
    if (addr >= m->fault_lo && addr <= m->fault_hi)
        return false;
    *out = m->use_pattern ? (uint8_t)(addr & 0xFFu) : m->fixed;
    return true;
}

/* ------------------------------------------------------------------ */
/* group: flags                                                        */
/* ------------------------------------------------------------------ */

/* adds r0, r0, r1 — carry and overflow, hand-computed */
static int case_flags_adds(void)
{
    static const uint8_t code[] = { 0x40, 0x18 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x100);
    cs_thumb_ir_inputs in;
    cs_thumb_ir_insn_info ii;
    cs_thumb_ir_node_info ni;
    uint32_t v;

    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_insn_count(g) == 1);
    TASK_CHECK(cs_thumb_ir_get_insn(g, 0, &ii));
    TASK_CHECK(ii.addr == 0x100);
    TASK_CHECK(ii.root_count == 5); /* r0 + N/Z/C/V */

    /* structural: result and flag roots have the expected operators */
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_reg(g, 0), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_ADD && ni.type == CS_THUMB_IR_TYPE_U32);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_N), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_MSB && ni.type == CS_THUMB_IR_TYPE_U1);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_Z), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_IS_ZERO);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_C), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_CARRY_ADD);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_V), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_OVF_ADD);

    /* set 1: 0xFFFFFFFF + 1 = 0, carry out, no overflow */
    inputs_zero(&in);
    in.regs[0] = 0xFFFFFFFFu; in.regs[1] = 1;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);

    /* set 2: 0x7FFFFFFF + 1 = 0x80000000, signed overflow, no carry */
    inputs_zero(&in);
    in.regs[0] = 0x7FFFFFFFu; in.regs[1] = 1;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0x80000000u);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 1);

    /* set 3: 5 + 0xFFFFFFF9 = 0xFFFFFFFE, negative, no carry, no overflow */
    inputs_zero(&in);
    in.regs[0] = 5; in.regs[1] = 0xFFFFFFF9u;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0xFFFFFFFEu);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);

    /* untouched register keeps the entry node and evaluates to the input */
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_reg(g, 5), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_REG && ni.value == 5);
    in.regs[5] = 0xCAFEBABEu;
    TASK_CHECK(get_reg(g, &in, 5, &v) && v == 0xCAFEBABEu);

    cs_thumb_ir_free(g);
    return 0;
}

/* subs / cmp — borrow (C = no borrow) and signed overflow */
static int case_flags_subs_cmp(void)
{
    static const uint8_t c_subs[] = { 0x52, 0x1e }; /* subs r2, r2, #1 */
    static const uint8_t c_cmpi[] = { 0x00, 0x2b }; /* cmp r3, #0 */
    static const uint8_t c_cmpr[] = { 0x9a, 0x42 }; /* cmp r2, r3 */
    cs_thumb_ir *g;
    cs_thumb_ir_inputs in;
    cs_thumb_ir_node_info ni;
    uint32_t v;

    g = lift_ok(c_subs, sizeof(c_subs), 0x100);
    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_C), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_CARRY_SUB);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_V), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_OVF_SUB);

    /* 0 - 1 = 0xFFFFFFFF: borrow (C=0), negative, no overflow */
    inputs_zero(&in);
    in.regs[2] = 0;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0xFFFFFFFFu);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);

    /* 0x80000000 - 1 = 0x7FFFFFFF: no borrow, signed overflow */
    inputs_zero(&in);
    in.regs[2] = 0x80000000u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0x7FFFFFFFu);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 1);

    /* 1 - 1 = 0: zero, no borrow, no overflow */
    inputs_zero(&in);
    in.regs[2] = 1;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);
    cs_thumb_ir_free(g);

    /* cmp r3, #0: flags only, no register write */
    g = lift_ok(c_cmpi, sizeof(c_cmpi), 0x100);
    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_reg(g, 3), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_REG && ni.value == 3);
    inputs_zero(&in);
    in.regs[3] = 0x80000000u;
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);
    inputs_zero(&in);
    in.regs[3] = 0;
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    cs_thumb_ir_free(g);

    /* cmp r2, r3 with 3 vs 5: 3 - 5 = 0xFFFFFFFE, borrow, negative */
    g = lift_ok(c_cmpr, sizeof(c_cmpr), 0x100);
    TASK_CHECK(g != NULL);
    inputs_zero(&in);
    in.regs[2] = 3; in.regs[3] = 5;
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);
    cs_thumb_ir_free(g);
    return 0;
}

/* MOVS updates only N/Z; a non-S MOV keeps every flag */
static int case_flags_movs_preserve(void)
{
    static const uint8_t c_movs[] = { 0x00, 0x20 }; /* movs r0, #0 */
    static const uint8_t c_mov[] = { 0x08, 0x46 };  /* mov r0, r1 */
    cs_thumb_ir *g;
    cs_thumb_ir_inputs in;
    cs_thumb_ir_node_info ni;
    uint32_t v;

    g = lift_ok(c_movs, sizeof(c_movs), 0x100);
    TASK_CHECK(g != NULL);
    /* C and V roots are still the entry flag nodes */
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_C), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_C);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_V), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_V);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_N), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_MSB);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_Z), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_IS_ZERO);
    /* entry N=1,Z=0,C=1,V=1: movs r0,#0 gives N=0,Z=1 and keeps C=1,V=1 */
    inputs_zero(&in);
    in.flags[CS_THUMB_IR_FLAG_N] = 1;
    in.flags[CS_THUMB_IR_FLAG_C] = 1;
    in.flags[CS_THUMB_IR_FLAG_V] = 1;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 1);
    cs_thumb_ir_free(g);

    /* mov r0, r1 (no S): all four flag roots are the entry nodes */
    g = lift_ok(c_mov, sizeof(c_mov), 0x100);
    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_N), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_N);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_Z), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_Z);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_C), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_C);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_V), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_V);
    inputs_zero(&in);
    in.regs[1] = 0xABCD1234u;
    in.flags[CS_THUMB_IR_FLAG_N] = 1;
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    in.flags[CS_THUMB_IR_FLAG_V] = 1;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0xABCD1234u);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 1);
    cs_thumb_ir_free(g);
    return 0;
}

/* MOVW/MOVT: MOVT preserves the low 16 bits */
static int case_flags_movw_movt(void)
{
    static const uint8_t code[] = {
        0x41, 0xf2, 0x34, 0x20, /* movw r0, #0x1234 */
        0xc5, 0xf2, 0x78, 0x60, /* movt r0, #0x5678 */
    };
    static const uint8_t c_movt[] = { 0xc5, 0xf2, 0x78, 0x60 };
    cs_thumb_ir *g;
    cs_thumb_ir_inputs in;
    cs_thumb_ir_insn_info ii;
    cs_thumb_ir_node_info ni, c0, c1;
    uint32_t v;

    g = lift_ok(code, sizeof(code), 0x9000);
    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_insn_count(g) == 2);
    TASK_CHECK(cs_thumb_ir_get_insn(g, 0, &ii));
    TASK_CHECK(ii.addr == 0x9000 && ii.root_count == 1);
    TASK_CHECK(ii.roots[0].kind == CS_THUMB_IR_ROOT_REG && ii.roots[0].index == 0);
    TASK_CHECK(cs_thumb_ir_get_insn(g, 1, &ii));
    TASK_CHECK(ii.addr == 0x9004 && ii.root_count == 1);

    inputs_zero(&in);
    in.regs[0] = 0xDEADBEEFu;
    /* per-instruction roots: after movw 0x00001234, after movt 0x56781234 */
    TASK_CHECK(cs_thumb_ir_get_insn(g, 0, &ii));
    TASK_CHECK(get_node(g, &in, ii.roots[0].node, &v) && v == 0x00001234u);
    TASK_CHECK(cs_thumb_ir_get_insn(g, 1, &ii));
    TASK_CHECK(get_node(g, &in, ii.roots[0].node, &v) && v == 0x56781234u);
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0x56781234u);

    /* movt root: OR(AND(old, 0xFFFF), 0x56780000) */
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_reg(g, 0), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_OR && ni.child_count == 2);
    TASK_CHECK(cs_thumb_ir_get_node(g, ni.child[0], &c0));
    TASK_CHECK(cs_thumb_ir_get_node(g, ni.child[1], &c1));
    TASK_CHECK(c0.op == CS_THUMB_IR_OP_AND);
    TASK_CHECK(c1.op == CS_THUMB_IR_OP_CONST && c1.value == 0x56780000u);

    /* flags untouched by movw/movt */
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_Z), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG);
    cs_thumb_ir_free(g);

    /* movt alone: entry r0 = 0xA5A55A5A keeps 0x5A5A in the low half */
    g = lift_ok(c_movt, sizeof(c_movt), 0x9000);
    TASK_CHECK(g != NULL);
    inputs_zero(&in);
    in.regs[0] = 0xA5A55A5Au;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0x56785A5Au);
    cs_thumb_ir_free(g);
    return 0;
}

/* graph-wide structural invariants */
static int case_flags_structure(void)
{
    static const uint8_t code[] = { 0x05, 0x20, 0x40, 0x18 }; /* movs r0,#5; adds r0,r0,r1 */
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x100);
    cs_thumb_ir_node_info ni;
    cs_thumb_ir_inputs in;
    uint32_t i, c, v;

    TASK_CHECK(g != NULL);
    /* entry nodes: 13 registers, 4 flags, 1 memory, 1 shared CONST 1 */
    TASK_CHECK(cs_thumb_ir_node_count(g) > 18);
    for (i = 0; i < 13; i++) {
        TASK_CHECK(cs_thumb_ir_get_node(g, i, &ni));
        TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_REG && ni.value == i);
        TASK_CHECK(ni.type == CS_THUMB_IR_TYPE_U32 && ni.child_count == 0);
    }
    for (i = 13; i < 17; i++) {
        TASK_CHECK(cs_thumb_ir_get_node(g, i, &ni));
        TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == i - 13);
        TASK_CHECK(ni.type == CS_THUMB_IR_TYPE_U1 && ni.child_count == 0);
    }
    TASK_CHECK(cs_thumb_ir_get_node(g, 17, &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_MEM && ni.type == CS_THUMB_IR_TYPE_MEM);
    TASK_CHECK(cs_thumb_ir_get_node(g, 18, &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_CONST && ni.value == 1 &&
               ni.type == CS_THUMB_IR_TYPE_U1);

    /* children always precede their users */
    for (i = 0; i < cs_thumb_ir_node_count(g); i++) {
        TASK_CHECK(cs_thumb_ir_get_node(g, i, &ni));
        for (c = 0; c < ni.child_count; c++)
            TASK_CHECK(ni.child[c] < i);
    }

    /* out-of-range queries fail cleanly */
    TASK_CHECK(cs_thumb_ir_root_reg(g, 13) == CS_THUMB_IR_INVALID_NODE);
    TASK_CHECK(cs_thumb_ir_root_flag(g, 4) == CS_THUMB_IR_INVALID_NODE);
    TASK_CHECK(!cs_thumb_ir_get_node(g, cs_thumb_ir_node_count(g), &ni));
    TASK_CHECK(!cs_thumb_ir_get_insn(g, cs_thumb_ir_insn_count(g),
                                      &(cs_thumb_ir_insn_info){0}));
    inputs_zero(&in);
    TASK_CHECK(cs_thumb_ir_eval_node(g, &in, cs_thumb_ir_root_mem(g), &v, NULL) ==
               CS_THUMB_IR_ERR_ARG);
    TASK_CHECK(cs_thumb_ir_eval_node(g, &in, cs_thumb_ir_node_count(g), &v, NULL) ==
               CS_THUMB_IR_ERR_ARG);

    cs_thumb_ir_free(g);
    return 0;
}

/* ------------------------------------------------------------------ */
/* group: predication                                                  */
/* ------------------------------------------------------------------ */

/* hand-computed condition truth table for ARM_CC_EQ..ARM_CC_LE (1..14) */
static int cond_true(int cc, int N, int Z, int C, int V)
{
    switch (cc) {
    case 1: return Z;               /* EQ */
    case 2: return !Z;              /* NE */
    case 3: return C;               /* HS */
    case 4: return !C;              /* LO */
    case 5: return N;               /* MI */
    case 6: return !N;              /* PL */
    case 7: return V;               /* VS */
    case 8: return !V;              /* VC */
    case 9: return C && !Z;         /* HI */
    case 10: return !C || Z;        /* LS */
    case 11: return N == V;         /* GE */
    case 12: return N != V;         /* LT */
    case 13: return !Z && N == V;   /* GT */
    case 14: return Z || N != V;    /* LE */
    default: return -1;
    }
}

/* it <cc>; mov<cc> r0, #1 — every condition EQ..LE against all 16 flag sets */
static int case_pred_truth_table(void)
{
    int cc, combo;
    for (cc = 1; cc <= 14; cc++) {
        uint8_t code[4] = { (uint8_t)(((cc - 1) << 4) | 0x08), 0xBF, 0x01, 0x20 };
        cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x2000);
        cs_thumb_ir_insn_info ii;
        cs_thumb_ir_node_info ni;
        TASK_CHECK(g != NULL);
        TASK_CHECK(cs_thumb_ir_insn_count(g) == 2);
        /* the IT itself executes unconditionally and writes nothing */
        TASK_CHECK(cs_thumb_ir_get_insn(g, 0, &ii));
        TASK_CHECK(ii.addr == 0x2000 && ii.root_count == 0);
        TASK_CHECK(cs_thumb_ir_get_node(g, ii.cond, &ni));
        TASK_CHECK(ni.op == CS_THUMB_IR_OP_CONST && ni.value == 1);
        /* the predicated mov has a U1 condition */
        TASK_CHECK(cs_thumb_ir_get_insn(g, 1, &ii));
        TASK_CHECK(ii.addr == 0x2002);
        TASK_CHECK(cs_thumb_ir_get_node(g, ii.cond, &ni));
        TASK_CHECK(ni.type == CS_THUMB_IR_TYPE_U1);
        /* for EQ the first instruction's condition is exactly the entry Z */
        if (cc == 1)
            TASK_CHECK(ii.cond == cs_thumb_ir_root_flag(g, CS_THUMB_IR_FLAG_Z));
        for (combo = 0; combo < 16; combo++) {
            cs_thumb_ir_inputs in;
            uint32_t v;
            int N = combo & 1, Z = (combo >> 1) & 1, C = (combo >> 2) & 1, V = (combo >> 3) & 1;
            inputs_zero(&in);
            in.flags[CS_THUMB_IR_FLAG_N] = (uint8_t)N;
            in.flags[CS_THUMB_IR_FLAG_Z] = (uint8_t)Z;
            in.flags[CS_THUMB_IR_FLAG_C] = (uint8_t)C;
            in.flags[CS_THUMB_IR_FLAG_V] = (uint8_t)V;
            in.regs[0] = 0;
            TASK_CHECK(get_reg(g, &in, 0, &v));
            TASK_CHECK(v == (uint32_t)(cond_true(cc, N, Z, C, V) ? 1 : 0));
        }
        cs_thumb_ir_free(g);
    }
    return 0;
}

/* ite eq; moveq r0,#1; movne r0,#2 — then/else selection */
static int case_pred_ite(void)
{
    static const uint8_t code[] = { 0x0c, 0xbf, 0x01, 0x20, 0x02, 0x20 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x2000);
    cs_thumb_ir_inputs in;
    cs_thumb_ir_insn_info ii1, ii2;
    uint32_t v;

    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_insn_count(g) == 3);
    TASK_CHECK(cs_thumb_ir_get_insn(g, 1, &ii1));
    TASK_CHECK(cs_thumb_ir_get_insn(g, 2, &ii2));
    /* the two arms use different condition nodes */
    TASK_CHECK(ii1.cond != ii2.cond);

    inputs_zero(&in);
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    in.regs[0] = 0xFFFFFFFFu;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 1);

    inputs_zero(&in);
    in.flags[CS_THUMB_IR_FLAG_Z] = 0;
    in.regs[0] = 0xFFFFFFFFu;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 2);

    cs_thumb_ir_free(g);
    return 0;
}

/* itt eq; cmpeq r0,#1; moveq r1,#7 — CMP inside the block updates the flags,
   so the second condition must use the version current at that point */
static int case_pred_cmp_chain(void)
{
    static const uint8_t code[] = { 0x04, 0xbf, 0x01, 0x28, 0x07, 0x21 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x2000);
    cs_thumb_ir_inputs in;
    cs_thumb_ir_insn_info ii;
    cs_thumb_ir_node_info ni;
    uint32_t v, i, zsel = CS_THUMB_IR_INVALID_NODE;

    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_insn_count(g) == 3);

    /* cmpeq's condition is the entry Z flag */
    TASK_CHECK(cs_thumb_ir_get_insn(g, 1, &ii));
    TASK_CHECK(ii.addr == 0x2002);
    TASK_CHECK(cs_thumb_ir_get_node(g, ii.cond, &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_FLAG && ni.value == CS_THUMB_IR_FLAG_Z);
    /* cmpeq rewrote all four flags */
    TASK_CHECK(ii.root_count == 4);
    for (i = 0; i < ii.root_count; i++) {
        TASK_CHECK(ii.roots[i].kind == CS_THUMB_IR_ROOT_FLAG);
        if (ii.roots[i].index == CS_THUMB_IR_FLAG_Z)
            zsel = ii.roots[i].node;
    }
    TASK_CHECK(zsel != CS_THUMB_IR_INVALID_NODE);

    /* moveq's condition is exactly the Z version produced by cmpeq,
       not the entry Z the block started with */
    TASK_CHECK(cs_thumb_ir_get_insn(g, 2, &ii));
    TASK_CHECK(ii.addr == 0x2004);
    TASK_CHECK(ii.cond == zsel);

    /* Z=1, r0=1: cmp runs (1-1=0 -> Z=1), moveq runs -> r1=7, C=1 */
    inputs_zero(&in);
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    in.regs[0] = 1; in.regs[1] = 0xAAAAu;
    TASK_CHECK(get_reg(g, &in, 1, &v) && v == 7);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_C, &v) && v == 1);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_N, &v) && v == 0);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_V, &v) && v == 0);

    /* Z=1, r0=2: cmp runs (2-1=1 -> Z=0), moveq skipped -> r1 kept */
    inputs_zero(&in);
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    in.regs[0] = 2; in.regs[1] = 0xAAAAu;
    TASK_CHECK(get_reg(g, &in, 1, &v) && v == 0xAAAAu);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);

    /* Z=0, r0=1: cmp skipped, flags keep entry Z=0, moveq skipped */
    inputs_zero(&in);
    in.regs[0] = 1; in.regs[1] = 0xAAAAu;
    TASK_CHECK(get_reg(g, &in, 1, &v) && v == 0xAAAAu);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);

    /* Z=0, r0=2: nothing executes */
    inputs_zero(&in);
    in.regs[0] = 2; in.regs[1] = 0xAAAAu;
    TASK_CHECK(get_reg(g, &in, 1, &v) && v == 0xAAAAu);
    TASK_CHECK(get_flag(g, &in, CS_THUMB_IR_FLAG_Z, &v) && v == 0);

    cs_thumb_ir_free(g);
    return 0;
}

/* it eq; ldreq r0, [r1] — a load under a false condition must not touch
   its address; a real read failure is reported and leaves no cache */
static int case_pred_false_load(void)
{
    static const uint8_t code[] = { 0x08, 0xbf, 0x08, 0x68 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x3000);
    cs_thumb_ir_inputs in;
    struct memcb m;
    uint32_t v, fail_addr;

    TASK_CHECK(g != NULL);

    /* false condition: no memory access at all, old value kept */
    memcb_init(&m);
    m.fail_all = 1; /* any access would fail — none must happen */
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0x1234u;
    in.regs[1] = 0xDEAD0000u;
    in.flags[CS_THUMB_IR_FLAG_Z] = 0;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0x1234u);
    TASK_CHECK(m.calls == 0);

    /* true condition, faulting address: explicit READ error at that address */
    memcb_init(&m);
    m.fault_lo = 0xDEAD0000u;
    m.fault_hi = 0xDEAD0003u;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[1] = 0xDEAD0000u;
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    fail_addr = 0;
    TASK_CHECK(cs_thumb_ir_eval_node(g, &in, cs_thumb_ir_root_reg(g, 0), &v,
                                     &fail_addr) == CS_THUMB_IR_ERR_READ);
    TASK_CHECK(fail_addr == 0xDEAD0000u);

    /* same graph after the failure: a good address evaluates fine */
    memcb_init(&m);
    m.use_pattern = 1; /* byte = addr & 0xff */
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[1] = 0x4000u;
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0x03020100u);
    TASK_CHECK(m.calls == 4);
    TASK_CHECK(m.addrs[0] == 0x4000u && m.addrs[1] == 0x4001u &&
               m.addrs[2] == 0x4002u && m.addrs[3] == 0x4003u);

    cs_thumb_ir_free(g);
    return 0;
}

/* error reporting: bad encodings, unsupported shapes, IT misuse, range */
static int case_pred_errors(void)
{
    static const uint8_t c_nested[] = { 0x04, 0xbf, 0x08, 0xbf, 0x01, 0x20 };
    static const uint8_t c_unterm[] = { 0x04, 0xbf, 0x01, 0x20 };
    static const uint8_t c_itonly[] = { 0x08, 0xbf };
    static const uint8_t c_ital[] = { 0xe8, 0xbf, 0x01, 0x20 };
    static const uint8_t c_bx[] = { 0x70, 0x47 };
    static const uint8_t c_ldrsp[] = { 0x00, 0x98 };
    static const uint8_t c_trunc[] = { 0x41, 0xf2 };
    static const uint8_t c_odd[] = { 0x05 };
    static const uint8_t c_nop[] = { 0x00, 0xbf };
    cs_thumb_ir *g = NULL;
    cs_thumb_ir_error err;
    cs_thumb_ir_inputs in;
    cs_thumb_ir_node_info ni;
    uint32_t v;

    /* nested IT: reported at the inner IT */
    TASK_CHECK(cs_thumb_ir_lift(c_nested, sizeof(c_nested), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_IT);
    TASK_CHECK(err.addr == 0x5002 && g == NULL);

    /* IT block not finished at fragment end: reported at the IT */
    TASK_CHECK(cs_thumb_ir_lift(c_unterm, sizeof(c_unterm), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_IT);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);
    TASK_CHECK(cs_thumb_ir_lift(c_itonly, sizeof(c_itonly), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_IT);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);

    /* IT with a condition outside EQ..LE */
    TASK_CHECK(cs_thumb_ir_lift(c_ital, sizeof(c_ital), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_UNSUPPORTED);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);

    /* control transfer and SP-based memory are unsupported */
    TASK_CHECK(cs_thumb_ir_lift(c_bx, sizeof(c_bx), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_UNSUPPORTED);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);
    TASK_CHECK(cs_thumb_ir_lift(c_ldrsp, sizeof(c_ldrsp), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_UNSUPPORTED);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);

    /* truncated wide instruction and lone byte: decode errors at the site */
    TASK_CHECK(cs_thumb_ir_lift(c_trunc, sizeof(c_trunc), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_DECODE);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);
    TASK_CHECK(cs_thumb_ir_lift(c_odd, sizeof(c_odd), 0x5000, &g, &err) ==
               CS_THUMB_IR_ERR_DECODE);
    TASK_CHECK(err.addr == 0x5000 && g == NULL);

    /* address range wrap around uint32 */
    TASK_CHECK(cs_thumb_ir_lift(c_nop, sizeof(c_nop), 0xFFFFFFFFu, &g, &err) ==
               CS_THUMB_IR_ERR_RANGE);
    TASK_CHECK(err.addr == 0xFFFFFFFFu && g == NULL);
    /* ... while a 2-byte instruction at 0xFFFFFFFE is exactly in range */
    TASK_CHECK(cs_thumb_ir_lift(c_nop, sizeof(c_nop), 0xFFFFFFFEu, &g, &err) ==
               CS_THUMB_IR_OK);
    TASK_CHECK(g != NULL);
    cs_thumb_ir_free(g);
    g = NULL;

    /* bad arguments */
    TASK_CHECK(cs_thumb_ir_lift(NULL, 4, 0x1000, &g, &err) == CS_THUMB_IR_ERR_ARG);
    TASK_CHECK(cs_thumb_ir_lift(c_nop, sizeof(c_nop), 0x1000, NULL, &err) ==
               CS_THUMB_IR_ERR_ARG);

    /* empty fragment: entry roots, no instructions */
    TASK_CHECK(cs_thumb_ir_lift(NULL, 0, 0x7000, &g, &err) == CS_THUMB_IR_OK);
    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_insn_count(g) == 0);
    TASK_CHECK(cs_thumb_ir_node_count(g) == 19);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_reg(g, 0), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_REG && ni.value == 0);
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_mem(g), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_MEM);
    inputs_zero(&in);
    in.regs[0] = 0xABCDu;
    TASK_CHECK(get_reg(g, &in, 0, &v) && v == 0xABCDu);
    cs_thumb_ir_free(g);
    return 0;
}

/* ------------------------------------------------------------------ */
/* group: memory                                                       */
/* ------------------------------------------------------------------ */

/* str r0,[r1]; ldr r2,[r1] — store-to-load forwarding, no re-read */
static int case_mem_forward(void)
{
    static const uint8_t code[] = { 0x08, 0x60, 0x0a, 0x68 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x8000);
    cs_thumb_ir_inputs in;
    cs_thumb_ir_insn_info ii;
    cs_thumb_ir_node_info ni;
    struct memcb m;
    uint32_t v, id, fail_addr;
    uint8_t b;
    int i, loads;

    TASK_CHECK(g != NULL);
    TASK_CHECK(cs_thumb_ir_insn_count(g) == 2);

    /* unconditional instructions use the shared CONST 1 condition */
    TASK_CHECK(cs_thumb_ir_get_insn(g, 0, &ii));
    TASK_CHECK(ii.addr == 0x8000 && ii.root_count == 1);
    TASK_CHECK(ii.roots[0].kind == CS_THUMB_IR_ROOT_MEM);
    TASK_CHECK(ii.roots[0].node == cs_thumb_ir_root_mem(g));
    TASK_CHECK(cs_thumb_ir_get_node(g, ii.cond, &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_CONST && ni.value == 1);
    TASK_CHECK(cs_thumb_ir_get_insn(g, 1, &ii));
    TASK_CHECK(ii.addr == 0x8002 && ii.root_count == 1);
    TASK_CHECK(ii.roots[0].kind == CS_THUMB_IR_ROOT_REG && ii.roots[0].index == 2);
    TASK_CHECK(ii.roots[0].node == cs_thumb_ir_root_reg(g, 2));

    /* memory root is a chain of 4 STORE8 over the entry memory */
    id = cs_thumb_ir_root_mem(g);
    for (i = 0; i < 4; i++) {
        TASK_CHECK(cs_thumb_ir_get_node(g, id, &ni));
        TASK_CHECK(ni.op == CS_THUMB_IR_OP_STORE8 && ni.type == CS_THUMB_IR_TYPE_MEM);
        TASK_CHECK(ni.child_count == 3);
        id = ni.child[0];
    }
    TASK_CHECK(cs_thumb_ir_get_node(g, id, &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_INPUT_MEM);

    /* the load result is assembled from exactly 4 LOAD8 nodes */
    loads = 0;
    for (id = 0; id < cs_thumb_ir_node_count(g); id++) {
        TASK_CHECK(cs_thumb_ir_get_node(g, id, &ni));
        if (ni.op == CS_THUMB_IR_OP_LOAD8) {
            TASK_CHECK(ni.type == CS_THUMB_IR_TYPE_U32);
            loads++;
        }
    }
    TASK_CHECK(loads == 4);

    /* forwarding: the stored value comes back, entry memory never read */
    memcb_init(&m);
    m.fail_all = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xDEADBEEFu;
    in.regs[1] = 0x1000u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0xDEADBEEFu);
    TASK_CHECK(m.calls == 0);

    /* final memory: little-endian bytes EF BE AD DE at 0x1000..0x1003 */
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x1000, &b, NULL) == CS_THUMB_IR_OK && b == 0xEF);
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x1001, &b, NULL) == CS_THUMB_IR_OK && b == 0xBE);
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x1002, &b, NULL) == CS_THUMB_IR_OK && b == 0xAD);
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x1003, &b, NULL) == CS_THUMB_IR_OK && b == 0xDE);
    TASK_CHECK(m.calls == 0);

    /* an uncovered address hits the callback and its failure is reported */
    fail_addr = 0;
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x2000, &b, &fail_addr) ==
               CS_THUMB_IR_ERR_READ);
    TASK_CHECK(fail_addr == 0x2000u);

    cs_thumb_ir_free(g);
    return 0;
}

/* str r0,[r1]; ldr r2,[r3] — aliasing decided only after substitution */
static int case_mem_alias(void)
{
    static const uint8_t code[] = { 0x08, 0x60, 0x1a, 0x68 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x8000);
    cs_thumb_ir_inputs in;
    struct memcb m;
    uint32_t v;

    TASK_CHECK(g != NULL);

    /* A: r3 == r1 after substitution -> full forward, no entry reads */
    memcb_init(&m);
    m.use_pattern = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xA1B2C3D4u;
    in.regs[1] = 0x2000u;
    in.regs[3] = 0x2000u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0xA1B2C3D4u);
    TASK_CHECK(m.calls == 0);

    /* B: disjoint addresses -> all four bytes from entry memory */
    memcb_init(&m);
    m.use_pattern = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xA1B2C3D4u;
    in.regs[1] = 0x2000u;
    in.regs[3] = 0x2004u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0x07060504u);
    TASK_CHECK(m.calls == 4);
    TASK_CHECK(m.addrs[0] == 0x2004u && m.addrs[1] == 0x2005u &&
               m.addrs[2] == 0x2006u && m.addrs[3] == 0x2007u);

    /* C: half overlap -> two stored bytes, two entry bytes */
    memcb_init(&m);
    m.use_pattern = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xA1B2C3D4u;
    in.regs[1] = 0x2000u;
    in.regs[3] = 0x2002u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0x0504A1B2u);
    TASK_CHECK(m.calls == 2);
    TASK_CHECK(m.addrs[0] == 0x2004u && m.addrs[1] == 0x2005u);

    /* re-run case A on the same graph: identical result, no stale cache */
    memcb_init(&m);
    m.use_pattern = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xA1B2C3D4u;
    in.regs[1] = 0x2000u;
    in.regs[3] = 0x2000u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0xA1B2C3D4u);
    TASK_CHECK(m.calls == 0);

    cs_thumb_ir_free(g);
    return 0;
}

/* unaligned store partially overlapping the load window; address wrap */
static int case_mem_unaligned_partial(void)
{
    static const uint8_t code[] = { 0x08, 0x60, 0x1a, 0x68 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x8000);
    cs_thumb_ir_inputs in;
    struct memcb m;
    uint32_t v;
    uint8_t b;

    TASK_CHECK(g != NULL);

    /* store at 0x1001..0x1004, load at 0x1000..0x1003:
       byte 0x1000 from entry (0x77), then D4 C3 B2 from the store */
    memcb_init(&m);
    m.fixed = 0x77;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xA1B2C3D4u;
    in.regs[1] = 0x1001u;
    in.regs[3] = 0x1000u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0xB2C3D477u);
    TASK_CHECK(m.calls == 1 && m.addrs[0] == 0x1000u);

    /* the byte just past the store comes from entry memory */
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x1004, &b, NULL) == CS_THUMB_IR_OK && b == 0xA1);
    TASK_CHECK(m.calls == 1);
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x1005, &b, NULL) == CS_THUMB_IR_OK && b == 0x77);
    TASK_CHECK(m.calls == 2 && m.addrs[1] == 0x1005u);

    /* store address 0xFFFFFFFF wraps: bytes at FF FF FF FF, 0, 1, 2 */
    memcb_init(&m);
    m.fail_all = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xA1B2C3D4u;
    in.regs[1] = 0xFFFFFFFFu;
    in.regs[3] = 0xFFFFFFFFu;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0xA1B2C3D4u);
    TASK_CHECK(m.calls == 0);

    cs_thumb_ir_free(g);
    return 0;
}

/* it eq; streq r0,[r1] — a conditional store selects between versions */
static int case_mem_cond_store(void)
{
    static const uint8_t code[] = { 0x08, 0xbf, 0x08, 0x60 };
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x8000);
    cs_thumb_ir_inputs in;
    cs_thumb_ir_node_info ni, t;
    struct memcb m;
    uint8_t b;

    TASK_CHECK(g != NULL);

    /* memory root: SELECT(cond, STORE8 chain, entry memory) */
    TASK_CHECK(cs_thumb_ir_get_node(g, cs_thumb_ir_root_mem(g), &ni));
    TASK_CHECK(ni.op == CS_THUMB_IR_OP_SELECT && ni.type == CS_THUMB_IR_TYPE_MEM);
    TASK_CHECK(ni.child_count == 3);
    TASK_CHECK(cs_thumb_ir_get_node(g, ni.child[0], &t));
    TASK_CHECK(t.type == CS_THUMB_IR_TYPE_U1);
    TASK_CHECK(cs_thumb_ir_get_node(g, ni.child[1], &t));
    TASK_CHECK(t.op == CS_THUMB_IR_OP_STORE8);
    TASK_CHECK(cs_thumb_ir_get_node(g, ni.child[2], &t));
    TASK_CHECK(t.op == CS_THUMB_IR_OP_INPUT_MEM);

    /* false condition: the old byte shows through, store does not mask it */
    memcb_init(&m);
    m.fixed = 0x55;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xFFFFFFFFu;
    in.regs[1] = 0x5000u;
    in.flags[CS_THUMB_IR_FLAG_Z] = 0;
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x5000, &b, NULL) == CS_THUMB_IR_OK && b == 0x55);
    TASK_CHECK(m.calls == 1 && m.addrs[0] == 0x5000u);

    /* true condition: stored bytes win, neighbours still come from entry */
    memcb_init(&m);
    m.fixed = 0x55;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0xFFFFFFFFu;
    in.regs[1] = 0x5000u;
    in.flags[CS_THUMB_IR_FLAG_Z] = 1;
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x5000, &b, NULL) == CS_THUMB_IR_OK && b == 0xFF);
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x5003, &b, NULL) == CS_THUMB_IR_OK && b == 0xFF);
    TASK_CHECK(m.calls == 0);
    TASK_CHECK(cs_thumb_ir_eval_mem8(g, &in, 0x5004, &b, NULL) == CS_THUMB_IR_OK && b == 0x55);
    TASK_CHECK(m.calls == 1 && m.addrs[0] == 0x5004u);

    cs_thumb_ir_free(g);
    return 0;
}

/* str r0,[r1,#4]; ldr r2,[r3,#4] — non-zero word offsets */
static int case_mem_word_offset(void)
{
    static const uint8_t code[] = { 0x48, 0x60, 0x5a, 0x68 }; /* str r0,[r1,#4]; ldr r2,[r3,#4] */
    cs_thumb_ir *g = lift_ok(code, sizeof(code), 0x8000);
    cs_thumb_ir_inputs in;
    struct memcb m;
    uint32_t v;

    TASK_CHECK(g != NULL);

    /* store to r1+4, load from r3+4: same effective address 0x2004 */
    memcb_init(&m);
    m.fail_all = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0x11223344u;
    in.regs[1] = 0x2000u;
    in.regs[3] = 0x2000u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0x11223344u);
    TASK_CHECK(m.calls == 0);

    /* different bases: effective addresses 0x2004 (store) vs 0x2008 (load),
       so the load reads entry memory bytes 08 09 0A 0B at 0x2008..0x200B */
    memcb_init(&m);
    m.use_pattern = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0x11223344u;
    in.regs[1] = 0x2000u;
    in.regs[3] = 0x2004u;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0x0B0A0908u);
    TASK_CHECK(m.calls == 4);
    TASK_CHECK(m.addrs[0] == 0x2008u && m.addrs[1] == 0x2009u &&
               m.addrs[2] == 0x200Au && m.addrs[3] == 0x200Bu);

    /* offset wraps the effective address: r1 = 0xFFFFFFFC, +4 -> 0 */
    memcb_init(&m);
    m.fail_all = 1;
    inputs_zero(&in);
    in.read8 = mem_read8;
    in.read8_ud = &m;
    in.regs[0] = 0x11223344u;
    in.regs[1] = 0xFFFFFFFCu;
    in.regs[3] = 0xFFFFFFFCu;
    TASK_CHECK(get_reg(g, &in, 2, &v) && v == 0x11223344u);
    TASK_CHECK(m.calls == 0);

    cs_thumb_ir_free(g);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "flags", "adds_carry_overflow", case_flags_adds },
    { "flags", "subs_cmp_borrow_overflow", case_flags_subs_cmp },
    { "flags", "movs_preserves_cv", case_flags_movs_preserve },
    { "flags", "movw_movt_keep_low16", case_flags_movw_movt },
    { "flags", "graph_structure", case_flags_structure },
    { "predication", "cond_truth_table", case_pred_truth_table },
    { "predication", "ite_then_else", case_pred_ite },
    { "predication", "cmp_updates_it_condition", case_pred_cmp_chain },
    { "predication", "false_load_no_fault", case_pred_false_load },
    { "predication", "lift_errors", case_pred_errors },
    { "memory", "store_load_forward", case_mem_forward },
    { "memory", "word_offset", case_mem_word_offset },
    { "memory", "symbolic_alias", case_mem_alias },
    { "memory", "unaligned_partial_overlap", case_mem_unaligned_partial },
    { "memory", "conditional_store_select", case_mem_cond_store },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
