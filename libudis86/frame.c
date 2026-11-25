/*
 * frame.c - static stack/RSP/RBP analysis for x86-64 basic blocks.
 *
 * See frame.h for the ownership and semantics contract.  This implementation
 * uses udis86 only as a byte decoder; all data-flow state is private.
 */
#include "frame.h"

#include "../udis86.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Errors and abstract values                                                 */
/* ------------------------------------------------------------------------- */

static void
set_error(ud_frame_error_t *error, int code, size_t block, size_t offset,
          const char *message)
{
    if (error == NULL)
        return;
    error->code = code;
    error->block = block;
    error->offset = offset;
    snprintf(error->message, sizeof(error->message), "%s",
             message != NULL ? message : "");
}

const char *
ud_frame_error_string(int code)
{
    switch (code) {
    case UD_FRAME_OK: return "ok";
    case UD_FRAME_ERR_ARGUMENT: return "invalid argument";
    case UD_FRAME_ERR_GRAPH: return "invalid graph";
    case UD_FRAME_ERR_RANGE: return "bad block range";
    case UD_FRAME_ERR_EDGE: return "bad block edge";
    case UD_FRAME_ERR_DECODE: return "invalid or truncated instruction";
    case UD_FRAME_ERR_UNSUPPORTED: return "unsupported instruction";
    case UD_FRAME_ERR_NOMEM: return "out of memory";
    default: return "unknown frame error";
    }
}

static ud_frame_value_t
value_make(int kind, int64_t offset)
{
    ud_frame_value_t v;
    v.kind = kind;
    v.offset = offset;
    return v;
}

static ud_frame_value_t value_unreached(void) { return value_make(UD_FRAME_UNREACHED, 0); }
static ud_frame_value_t value_unknown(void) { return value_make(UD_FRAME_UNKNOWN, 0); }
static ud_frame_value_t value_relative(int64_t d) { return value_make(UD_FRAME_RELATIVE, d); }

static int
value_equal(ud_frame_value_t a, ud_frame_value_t b)
{
    return a.kind == b.kind &&
           (a.kind != UD_FRAME_RELATIVE || a.offset == b.offset);
}

static int
state_equal(const ud_frame_state_t *a, const ud_frame_state_t *b)
{
    return value_equal(a->rsp, b->rsp) && value_equal(a->rbp, b->rbp);
}

static ud_frame_value_t
value_merge(ud_frame_value_t a, ud_frame_value_t b)
{
    if (a.kind == UD_FRAME_UNREACHED)
        return b;
    if (b.kind == UD_FRAME_UNREACHED)
        return a;
    if (value_equal(a, b))
        return a;
    return value_unknown();
}

static ud_frame_state_t
state_merge(const ud_frame_state_t *a, const ud_frame_state_t *b)
{
    ud_frame_state_t out;
    out.rsp = value_merge(a->rsp, b->rsp);
    out.rbp = value_merge(a->rbp, b->rbp);
    return out;
}

static int
safe_add64(int64_t a, int64_t b, int64_t *result)
{
    if (b > 0 && a > INT64_MAX - b)
        return 0;
    if (b < 0 && a < INT64_MIN - b)
        return 0;
    *result = a + b;
    return 1;
}

static int
safe_sub64(int64_t a, int64_t b, int64_t *result)
{
    if (b > 0 && a < INT64_MIN + b)
        return 0;
    if (b < 0 && a > INT64_MAX + b)
        return 0;
    *result = a - b;
    return 1;
}

static int
safe_mul64(int64_t a, int64_t b, int64_t *result)
{
    if (a == 0 || b == 0) {
        *result = 0;
        return 1;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b)
                return 0;
        } else {
            if (b < INT64_MIN / a)
                return 0;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b)
                return 0;
        } else {
            if (a < -INT64_MAX || b < -INT64_MAX)
                return 0;
            if (-a > INT64_MAX / -b)
                return 0;
        }
    }
    *result = a * b;
    return 1;
}

static ud_frame_value_t
value_add(ud_frame_value_t v, int64_t d)
{
    int64_t r;
    if (v.kind != UD_FRAME_RELATIVE)
        return v; /* UNKNOWN and UNREACHED preserve their lattice kind. */
    if (!safe_add64(v.offset, d, &r))
        return value_unknown();
    return value_relative(r);
}

static ud_frame_value_t
value_sub(ud_frame_value_t v, int64_t d)
{
    int64_t r;
    if (v.kind != UD_FRAME_RELATIVE)
        return v;
    if (!safe_sub64(v.offset, d, &r))
        return value_unknown();
    return value_relative(r);
}

/* ------------------------------------------------------------------------- */
/* Decoded instruction cache                                                  */
/* ------------------------------------------------------------------------- */

typedef struct frame_insn {
    size_t offset;
    unsigned int length;
    enum ud_mnemonic_code mnemonic;
    unsigned int opr_mode;
    uint8_t pfx_adr;
    enum ud_type pfx_seg;
    struct ud_operand opr[4];
} frame_insn_t;

typedef struct frame_block {
    int decoded;
    size_t insn_count;
    frame_insn_t *insns;
    int suppress_successors; /* set by RET */
} frame_block_t;

typedef struct frame_build {
    ud_frame_report_t *report;
    size_t insn_cap;
    size_t access_cap;
    ud_frame_error_t *error;
} frame_build_t;

/* ------------------------------------------------------------------------- */
/* Register and operand helpers                                               */
/* ------------------------------------------------------------------------- */

enum reg_sel {
    REG_OTHER = 0,
    REG_RSP,
    REG_RBP
};

static enum reg_sel
reg_class(enum ud_type r)
{
    switch (r) {
    case UD_R_RSP:
    case UD_R_ESP:
    case UD_R_SP:
    case UD_R_SPL:
        return REG_RSP;
    case UD_R_RBP:
    case UD_R_EBP:
    case UD_R_BP:
    case UD_R_BPL:
        return REG_RBP;
    default:
        return REG_OTHER;
    }
}

static int
reg_is_full64(enum ud_type r)
{
    return r == UD_R_RSP || r == UD_R_RBP;
}

static ud_frame_value_t
reg_read_full64(const ud_frame_state_t *state, enum ud_type r)
{
    if (r == UD_R_RSP)
        return state->rsp;
    if (r == UD_R_RBP)
        return state->rbp;
    return value_unknown();
}

static void
reg_kill(ud_frame_state_t *state, enum reg_sel sel)
{
    if (sel == REG_RSP)
        state->rsp = value_unknown();
    else if (sel == REG_RBP)
        state->rbp = value_unknown();
}

static int64_t
signed_immediate(const struct ud_operand *op)
{
    switch (op->size) {
    case 8:  return (int64_t)op->lval.sbyte;
    case 16: return (int64_t)(int16_t)op->lval.uword;
    case 32: return (int64_t)(int32_t)op->lval.udword;
    case 64: return (int64_t)op->lval.sqword;
    default: return (int64_t)op->lval.sdword;
    }
}

static int64_t
memory_displacement(const struct ud_operand *op)
{
    switch (op->offset) {
    case 8:  return (int64_t)op->lval.sbyte;
    case 16: return (int64_t)(int16_t)op->lval.uword;
    case 32: return (int64_t)(int32_t)op->lval.udword;
    case 64: return (int64_t)op->lval.sqword;
    default: return 0;
    }
}

/*
 * Return RELATIVE(d) only when the effective address is exactly S+d.
 * Any untracked or unknown base/index, address-size override, or FS/GS segment
 * makes the result unknown.
 */
static ud_frame_value_t
memory_address(const frame_insn_t *insn, const struct ud_operand *mem,
               const ud_frame_state_t *state)
{
    int coefficient = 0;
    int64_t displacement = 0;
    ud_frame_value_t part;
    int64_t scaled;

    if (insn->pfx_adr != 0)
        return value_unknown();
    if (insn->pfx_seg == UD_R_FS || insn->pfx_seg == UD_R_GS)
        return value_unknown();

    if (mem->base != UD_NONE) {
        if (mem->base != UD_R_RSP && mem->base != UD_R_RBP)
            return value_unknown();
        part = reg_read_full64(state, mem->base);
        if (part.kind != UD_FRAME_RELATIVE)
            return value_unknown();
        coefficient = 1;
        displacement = part.offset;
    }

    if (mem->index != UD_NONE) {
        unsigned int scale = mem->scale != 0 ? mem->scale : 1u;
        if (mem->index != UD_R_RSP && mem->index != UD_R_RBP)
            return value_unknown();
        part = reg_read_full64(state, mem->index);
        if (part.kind != UD_FRAME_RELATIVE)
            return value_unknown();
        if (!safe_mul64(part.offset, (int64_t)scale, &scaled))
            return value_unknown();
        coefficient += (int)scale;
        if (!safe_add64(displacement, scaled, &displacement))
            return value_unknown();
    }

    if (!safe_add64(displacement, memory_displacement(mem), &displacement))
        return value_unknown();

    if (coefficient != 1)
        return value_unknown();
    return value_relative(displacement);
}

/* ------------------------------------------------------------------------- */
/* Report construction                                                        */
/* ------------------------------------------------------------------------- */

static int
add_insn(frame_build_t *build, const frame_insn_t *insn, size_t block_index,
         size_t block_begin, const ud_frame_state_t *before)
{
    ud_frame_report_t *r = build->report;
    ud_frame_insn_t *p;

    if (r->insn_count == build->insn_cap) {
        size_t new_cap = build->insn_cap ? build->insn_cap * 2 : 16;
        void *mem = realloc(r->insns, new_cap * sizeof(*r->insns));
        if (mem == NULL) {
            set_error(build->error, UD_FRAME_ERR_NOMEM, block_index,
                      insn->offset, "out of memory");
            return -1;
        }
        r->insns = mem;
        build->insn_cap = new_cap;
    }

    p = &r->insns[r->insn_count++];
    memset(p, 0, sizeof(*p));
    p->block_index = block_index;
    p->block_begin = block_begin;
    p->offset = insn->offset;
    p->length = insn->length;
    p->mnemonic = (unsigned int)insn->mnemonic;
    p->before = *before;
    p->access_start = r->access_count;
    p->access_count = 0;
    p->accesses = NULL;
    return (int)(r->insn_count - 1);
}

static int
add_access(frame_build_t *build, const frame_insn_t *insn,
           size_t block_index, size_t block_begin, unsigned int operand_index,
           int kind, unsigned int direction, unsigned int width_bytes,
           const ud_frame_value_t *address)
{
    ud_frame_report_t *r;
    ud_frame_access_t *a;

    if (build == NULL || build->report == NULL)
        return 0; /* abstract propagation only */
    r = build->report;

    if (r->access_count == build->access_cap) {
        size_t new_cap = build->access_cap ? build->access_cap * 2 : 16;
        void *mem = realloc(r->accesses, new_cap * sizeof(*r->accesses));
        if (mem == NULL) {
            set_error(build->error, UD_FRAME_ERR_NOMEM, block_index,
                      insn->offset, "out of memory");
            return -1;
        }
        r->accesses = mem;
        build->access_cap = new_cap;
    }

    a = &r->accesses[r->access_count++];
    memset(a, 0, sizeof(*a));
    a->block_index = block_index;
    a->block_begin = block_begin;
    a->insn_offset = insn->offset;
    a->length = insn->length;
    a->operand_index = operand_index;
    a->kind = kind;
    a->direction = direction;
    a->width = width_bytes;
    a->width_bits = width_bytes * 8u;
    a->address_known = address->kind == UD_FRAME_RELATIVE;
    a->address = a->address_known ? address->offset : 0;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Instruction transfer                                                       */
/* ------------------------------------------------------------------------- */

static int
is_jcc(enum ud_mnemonic_code m)
{
    switch (m) {
    case UD_Ija: case UD_Ijae: case UD_Ijb: case UD_Ijbe:
    case UD_Ijg: case UD_Ijge: case UD_Ijl: case UD_Ijle:
    case UD_Ijno: case UD_Ijnp: case UD_Ijns: case UD_Ijnz:
    case UD_Ijo: case UD_Ijp: case UD_Ijs: case UD_Ijz:
    case UD_Ijcxz: case UD_Ijecxz: case UD_Ijrcxz:
        return 1;
    default:
        return 0;
    }
}

static int
is_supported(enum ud_mnemonic_code m)
{
    switch (m) {
    case UD_Inop:
    case UD_Imov:
    case UD_Ilea:
    case UD_Iadd:
    case UD_Isub:
    case UD_Iand:
    case UD_Icmp:
    case UD_Itest:
    case UD_Icall:
    case UD_Ijmp:
    case UD_Iret:
    case UD_Ileave:
    case UD_Ipush:
    case UD_Ipop:
        return 1;
    default:
        return is_jcc(m);
    }
}

static int
record_access(frame_build_t *build, const frame_insn_t *insn,
              size_t insn_index, size_t block_index, size_t block_begin,
              unsigned int operand_index, int kind, unsigned int direction,
              unsigned int width_bytes, const ud_frame_value_t *address)
{
    int rc = add_access(build, insn, block_index, block_begin, operand_index,
                        kind, direction, width_bytes, address);
    if (rc != 0)
        return rc;
    if (build != NULL && build->report != NULL)
        build->report->insns[insn_index].access_count++;
    return 0;
}

static int
apply_insn(const frame_insn_t *insn, ud_frame_state_t *state,
           size_t block_index, size_t block_begin,
           frame_build_t *build, size_t insn_index, int *stop_propagation,
           ud_frame_error_t *error)
{
    enum ud_mnemonic_code m = insn->mnemonic;
    const struct ud_operand *op0 = &insn->opr[0];
    int rc;

    if (!is_supported(m)) {
        set_error(error, UD_FRAME_ERR_UNSUPPORTED, block_index, insn->offset,
                  "unsupported instruction in reachable block");
        return -1;
    }

    if (m == UD_Inop || m == UD_Icmp || m == UD_Itest || m == UD_Ijmp ||
        is_jcc(m)) {
        if (m == UD_Ijmp && op0->type == UD_OP_PTR) {
            set_error(error, UD_FRAME_ERR_UNSUPPORTED, block_index,
                      insn->offset, "far jmp is not supported");
            return -1;
        }
        return 0;
    }

    if (m == UD_Imov) {
        unsigned int i;
        enum reg_sel sel;

        for (i = 0; i < 4; ++i) {
            const struct ud_operand *mem = &insn->opr[i];
            ud_frame_value_t address;
            unsigned int direction;

            if (mem->type != UD_OP_MEM)
                continue;
            direction = (i == 0) ? UD_FRAME_ACCESS_WRITE
                                 : UD_FRAME_ACCESS_READ;
            address = memory_address(insn, mem, state);
            rc = record_access(build, insn, insn_index, block_index,
                               block_begin, i, UD_FRAME_ACCESS_EXPLICIT,
                               direction, mem->size / 8u, &address);
            if (rc != 0)
                return rc;
        }

        if (op0->type == UD_OP_REG) {
            sel = reg_class(op0->base);
            if (sel != REG_OTHER) {
                if (reg_is_full64(op0->base)) {
                    const struct ud_operand *src = &insn->opr[1];
                    ud_frame_value_t next = value_unknown();
                    if (src->type == UD_OP_REG &&
                        reg_is_full64(src->base))
                        next = reg_read_full64(state, src->base);
                    if (sel == REG_RSP)
                        state->rsp = next;
                    else
                        state->rbp = next;
                } else {
                    reg_kill(state, sel);
                }
            }
        }
        return 0;
    }

    if (m == UD_Ilea) {
        const struct ud_operand *src = &insn->opr[1];
        ud_frame_value_t calculated = value_unknown();
        enum reg_sel sel = op0->type == UD_OP_REG ? reg_class(op0->base)
                                                  : REG_OTHER;

        if (src->type == UD_OP_MEM)
            calculated = memory_address(insn, src, state);
        rc = record_access(build, insn, insn_index, block_index, block_begin,
                           1, UD_FRAME_ACCESS_LEA, 0, 0, &calculated);
        if (rc != 0)
            return rc;

        if (sel != REG_OTHER) {
            if (reg_is_full64(op0->base)) {
                ud_frame_value_t next = value_unknown();
                if (src->type == UD_OP_MEM &&
                    insn->pfx_adr == 0 &&
                    src->index == UD_NONE &&
                    (src->base == UD_R_RSP || src->base == UD_R_RBP)) {
                    next = value_add(reg_read_full64(state, src->base),
                                     memory_displacement(src));
                }
                if (sel == REG_RSP)
                    state->rsp = next;
                else
                    state->rbp = next;
            } else {
                reg_kill(state, sel);
            }
        }
        return 0;
    }

    if (m == UD_Iadd || m == UD_Isub) {
        if (op0->type == UD_OP_REG) {
            enum reg_sel sel = reg_class(op0->base);
            if (sel != REG_OTHER) {
                if (!reg_is_full64(op0->base)) {
                    reg_kill(state, sel);
                } else if (insn->opr[1].type == UD_OP_IMM ||
                           insn->opr[1].type == UD_OP_CONST) {
                    int64_t imm = signed_immediate(&insn->opr[1]);
                    ud_frame_value_t cur = sel == REG_RSP ? state->rsp
                                                          : state->rbp;
                    ud_frame_value_t next = m == UD_Iadd
                                                ? value_add(cur, imm)
                                                : value_sub(cur, imm);
                    if (sel == REG_RSP)
                        state->rsp = next;
                    else
                        state->rbp = next;
                } else {
                    reg_kill(state, sel);
                }
            }
        }
        return 0;
    }

    if (m == UD_Iand) {
        if (op0->type == UD_OP_REG)
            reg_kill(state, reg_class(op0->base));
        return 0;
    }

    if (m == UD_Ipush) {
        ud_frame_value_t slot;
        if (insn->opr_mode != 64 ||
            !((op0->type == UD_OP_REG && op0->size == 64) ||
              op0->type == UD_OP_IMM)) {
            set_error(error, UD_FRAME_ERR_UNSUPPORTED, block_index,
                      insn->offset, "only 64-bit register/immediate push is supported");
            return -1;
        }
        slot = value_add(state->rsp, -8);
        rc = record_access(build, insn, insn_index, block_index, block_begin,
                           UD_FRAME_NO_OPERAND, UD_FRAME_ACCESS_PUSH,
                           UD_FRAME_ACCESS_WRITE, 8, &slot);
        if (rc != 0)
            return rc;
        state->rsp = slot;
        return 0;
    }

    if (m == UD_Ipop) {
        enum reg_sel sel;
        ud_frame_value_t next_rsp;
        if (insn->opr_mode != 64 || op0->type != UD_OP_REG ||
            op0->size != 64) {
            set_error(error, UD_FRAME_ERR_UNSUPPORTED, block_index,
                      insn->offset, "only 64-bit register pop is supported");
            return -1;
        }
        rc = record_access(build, insn, insn_index, block_index, block_begin,
                           UD_FRAME_NO_OPERAND, UD_FRAME_ACCESS_POP,
                           UD_FRAME_ACCESS_READ, 8, &state->rsp);
        if (rc != 0)
            return rc;

        sel = reg_class(op0->base);
        next_rsp = value_add(state->rsp, 8);
        if (sel == REG_RSP) {
            /* POP RSP loads RSP from memory; it is not RSP_old + 8. */
            state->rsp = value_unknown();
        } else {
            state->rsp = next_rsp;
        }
        if (sel == REG_RBP)
            state->rbp = value_unknown();
        return 0;
    }

    if (m == UD_Icall) {
        ud_frame_value_t slot;
        if (op0->type == UD_OP_PTR) {
            set_error(error, UD_FRAME_ERR_UNSUPPORTED, block_index,
                      insn->offset, "far call is not supported");
            return -1;
        }
        slot = value_add(state->rsp, -8);
        rc = record_access(build, insn, insn_index, block_index, block_begin,
                           UD_FRAME_NO_OPERAND, UD_FRAME_ACCESS_CALL,
                           UD_FRAME_ACCESS_WRITE, 8, &slot);
        if (rc != 0)
            return rc;
        /* The normal return convention makes the net RSP change zero. */
        return 0;
    }

    if (m == UD_Iret) {
        ud_frame_value_t next_rsp;
        rc = record_access(build, insn, insn_index, block_index, block_begin,
                           UD_FRAME_NO_OPERAND, UD_FRAME_ACCESS_RET,
                           UD_FRAME_ACCESS_READ, 8, &state->rsp);
        if (rc != 0)
            return rc;
        next_rsp = value_add(state->rsp, 8);
        if (insn->opr[0].type == UD_OP_IMM ||
            insn->opr[0].type == UD_OP_CONST) {
            next_rsp = value_add(next_rsp, signed_immediate(&insn->opr[0]));
        }
        state->rsp = next_rsp;
        if (stop_propagation != NULL)
            *stop_propagation = 1;
        return 0;
    }

    if (m == UD_Ileave) {
        ud_frame_value_t restored_rsp;
        rc = record_access(build, insn, insn_index, block_index, block_begin,
                           UD_FRAME_NO_OPERAND, UD_FRAME_ACCESS_LEAVE,
                           UD_FRAME_ACCESS_READ, 8, &state->rbp);
        if (rc != 0)
            return rc;
        restored_rsp = value_add(state->rbp, 8);
        state->rsp = restored_rsp;
        state->rbp = value_unknown();
        return 0;
    }

    set_error(error, UD_FRAME_ERR_UNSUPPORTED, block_index, insn->offset,
              "unsupported instruction");
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Decoding and analysis                                                      */
/* ------------------------------------------------------------------------- */

static int
decode_block(frame_block_t *fb, const uint8_t *code,
             const ud_frame_block_t *desc, size_t block_index,
             ud_frame_error_t *error)
{
    const size_t length = desc->end - desc->begin;
    size_t consumed = 0;
    ud_t decoder;

    fb->decoded = 1;
    if (length == 0)
        return 0;

    ud_init(&decoder);
    ud_set_mode(&decoder, 64);
    ud_set_input_buffer(&decoder, code + desc->begin, length);
    ud_set_pc(&decoder, (uint64_t)desc->begin);

    while (consumed < length) {
        unsigned int n = ud_decode(&decoder);
        size_t at = desc->begin + consumed;
        frame_insn_t *grown;
        frame_insn_t *insn;

        if (n == 0 || decoder.error ||
            decoder.mnemonic == UD_Iinvalid || (size_t)n > length - consumed) {
            set_error(error, UD_FRAME_ERR_DECODE, block_index, at,
                      "reachable invalid or truncated instruction");
            return -1;
        }

        grown = realloc(fb->insns,
                        (fb->insn_count + 1) * sizeof(*fb->insns));
        if (grown == NULL) {
            set_error(error, UD_FRAME_ERR_NOMEM, block_index, at,
                      "out of memory while decoding");
            return -1;
        }
        fb->insns = grown;
        insn = &fb->insns[fb->insn_count++];
        memset(insn, 0, sizeof(*insn));

        insn->offset = at;
        insn->length = n;
        insn->mnemonic = decoder.mnemonic;
        insn->opr_mode = decoder.opr_mode;
        insn->pfx_adr = decoder.pfx_adr;
        insn->pfx_seg = (enum ud_type)decoder.pfx_seg;
        memcpy(insn->opr, decoder.operand, sizeof(insn->opr));

        consumed += n;
    }

    if (consumed != length) {
        set_error(error, UD_FRAME_ERR_DECODE, block_index, desc->begin,
                  "block does not end on an instruction boundary");
        return -1;
    }
    return 0;
}

static int
block_less(const ud_frame_graph_t *graph, size_t a, size_t b)
{
    const ud_frame_block_t *ba = &graph->blocks[a];
    const ud_frame_block_t *bb = &graph->blocks[b];
    if (ba->begin != bb->begin)
        return ba->begin < bb->begin;
    if (ba->end != bb->end)
        return ba->end < bb->end;
    return a < b;
}

static ud_frame_state_t
compute_entry(size_t target, size_t entry,
              const ud_frame_state_t *out_states,
              const unsigned char *evaluated,
              const frame_block_t *blocks,
              const size_t *pred_data,
              const size_t *pred_offset)
{
    ud_frame_state_t result;
    size_t p;

    result.rsp = value_unreached();
    result.rbp = value_unreached();

    if (target == entry) {
        ud_frame_state_t boundary;
        boundary.rsp = value_relative(0); /* S */
        boundary.rbp = value_unknown();
        result = state_merge(&result, &boundary);
    }

    for (p = pred_offset[target]; p < pred_offset[target + 1]; ++p) {
        size_t pred = pred_data[p];
        if (!evaluated[pred] || blocks[pred].suppress_successors)
            continue;
        result = state_merge(&result, &out_states[pred]);
    }
    return result;
}

ud_frame_report_t *
ud_frame_analyze(const uint8_t *code, size_t code_size,
                 const ud_frame_graph_t *graph, ud_frame_error_t *error)
{
    size_t n, i, k;
    size_t *order = NULL;
    frame_block_t *blocks = NULL;
    size_t edge_count = 0;
    size_t *pred_count = NULL;
    size_t *pred_offset = NULL;
    size_t *pred_data = NULL;
    size_t *pred_cursor = NULL;
    ud_frame_state_t *entry_states = NULL;
    ud_frame_state_t *evaluated_states = NULL;
    ud_frame_state_t *out_states = NULL;
    unsigned char *evaluated = NULL;
    unsigned char *queued = NULL;
    size_t *stack = NULL;
    size_t stack_count = 0;
    ud_frame_report_t *report = NULL;
    frame_build_t build;
    ud_frame_report_t *result = NULL;

    memset(&build, 0, sizeof(build));
    build.error = error;
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->code = UD_FRAME_OK;
    }

    if (graph == NULL || (code == NULL && code_size != 0)) {
        set_error(error, UD_FRAME_ERR_ARGUMENT, UD_FRAME_NO_BLOCK, 0,
                  "NULL analyzer argument");
        return NULL;
    }
    n = graph->block_count;
    if (n == 0 || graph->blocks == NULL || graph->entry >= n) {
        set_error(error, UD_FRAME_ERR_GRAPH, UD_FRAME_NO_BLOCK, 0,
                  "graph must contain blocks and a valid entry");
        return NULL;
    }

    /* Validate ranges and all caller edges before publishing anything. */
    for (i = 0; i < n; ++i) {
        const ud_frame_block_t *b = &graph->blocks[i];
        size_t error_offset;

        if (b->begin > b->end || b->end > code_size) {
            error_offset = b->begin > b->end ? b->begin : b->end;
            set_error(error, UD_FRAME_ERR_RANGE, i, error_offset,
                      "block range is outside the buffer or malformed");
            return NULL;
        }
        if (b->successor_count != 0 && b->successors == NULL) {
            set_error(error, UD_FRAME_ERR_EDGE, i, b->begin,
                      "block has a NULL successor array");
            return NULL;
        }
        for (k = 0; k < b->successor_count; ++k) {
            if (b->successors[k] >= n) {
                set_error(error, UD_FRAME_ERR_EDGE, i, b->begin,
                          "successor block does not exist");
                return NULL;
            }
        }
        if (b->successor_count > SIZE_MAX - edge_count) {
            set_error(error, UD_FRAME_ERR_GRAPH, i, b->begin,
                      "too many graph edges");
            return NULL;
        }
        edge_count += b->successor_count;
    }

    order = calloc(n, sizeof(*order));
    if (order == NULL)
        goto nomem_early;
    for (i = 0; i < n; ++i)
        order[i] = i;

    /* Insertion sort keeps this C99 and avoids a non-portable qsort context. */
    for (i = 1; i < n; ++i) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 && block_less(graph, key, order[j - 1])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    {
        int have_interval = 0;
        size_t max_end = 0;
        for (i = 0; i < n; ++i) {
            size_t bi = order[i];
            const ud_frame_block_t *b = &graph->blocks[bi];
            if (b->begin == b->end)
                continue;
            if (have_interval && b->begin < max_end) {
                set_error(error, UD_FRAME_ERR_RANGE, bi, b->begin,
                          "block ranges overlap");
                free(order);
                return NULL;
            }
            if (!have_interval || b->end > max_end)
                max_end = b->end;
            have_interval = 1;
        }
    }

    blocks = calloc(n, sizeof(*blocks));
    pred_count = calloc(n, sizeof(*pred_count));
    pred_offset = calloc(n + 1, sizeof(*pred_offset));
    pred_data = calloc(edge_count ? edge_count : 1, sizeof(*pred_data));
    pred_cursor = calloc(n, sizeof(*pred_cursor));
    entry_states = calloc(n, sizeof(*entry_states));
    evaluated_states = calloc(n, sizeof(*evaluated_states));
    out_states = calloc(n, sizeof(*out_states));
    evaluated = calloc(n, sizeof(*evaluated));
    queued = calloc(n, sizeof(*queued));
    stack = calloc(n ? n : 1, sizeof(*stack));
    if (!blocks || !pred_count || !pred_offset || !pred_data ||
        !pred_cursor || !entry_states || !evaluated_states || !out_states ||
        !evaluated || !queued || !stack)
        goto nomem_early;

    for (i = 0; i < n; ++i) {
        for (k = 0; k < graph->blocks[i].successor_count; ++k)
            pred_count[graph->blocks[i].successors[k]]++;
    }
    pred_offset[0] = 0;
    for (i = 0; i < n; ++i)
        pred_offset[i + 1] = pred_offset[i] + pred_count[i];
    for (i = 0; i < n; ++i)
        pred_cursor[i] = pred_offset[i];
    for (i = 0; i < n; ++i) {
        const ud_frame_block_t *b = &graph->blocks[i];
        for (k = 0; k < b->successor_count; ++k) {
            size_t s = b->successors[k];
            pred_data[pred_cursor[s]++] = i;
        }
    }

    /*
     * Work-list fixed point.  Entries are recomputed from every arrived
     * predecessor, so changing a back-edge contribution cannot leave a stale
     * first-merge value behind.
     */
    stack[stack_count++] = graph->entry;
    queued[graph->entry] = 1;

    while (stack_count != 0) {
        size_t bi = stack[--stack_count];
        ud_frame_state_t current;
        ud_frame_state_t simulated;
        int changed_input;
        int stop = 0;
        size_t j;

        queued[bi] = 0;
        current = compute_entry(bi, graph->entry, out_states, evaluated,
                                blocks, pred_data, pred_offset);

        changed_input = !evaluated[bi] ||
                        !state_equal(&current, &evaluated_states[bi]);
        entry_states[bi] = current;
        if (!changed_input)
            continue;

        if (!blocks[bi].decoded &&
            decode_block(&blocks[bi], code, &graph->blocks[bi], bi,
                         error) != 0)
            goto fail;

        simulated = current;
        for (j = 0; j < blocks[bi].insn_count; ++j) {
            if (apply_insn(&blocks[bi].insns[j], &simulated, bi,
                           graph->blocks[bi].begin, NULL, 0, &stop,
                           error) != 0)
                goto fail;
        }

        blocks[bi].suppress_successors = stop;
        out_states[bi] = simulated;
        evaluated[bi] = 1;
        evaluated_states[bi] = current;

        if (stop)
            continue;
        for (j = 0; j < graph->blocks[bi].successor_count; ++j) {
            size_t s = graph->blocks[bi].successors[j];
            if (!queued[s] && stack_count < n) {
                stack[stack_count++] = s;
                queued[s] = 1;
            }
        }
    }

    report = calloc(1, sizeof(*report));
    if (report == NULL)
        goto nomem_early;
    build.report = report;

    report->block_count = n;
    report->blocks = calloc(n, sizeof(*report->blocks));
    if (report->blocks == NULL) {
        set_error(error, UD_FRAME_ERR_NOMEM, UD_FRAME_NO_BLOCK, 0,
                  "out of memory while publishing report");
        goto fail;
    }

    for (i = 0; i < n; ++i) {
        size_t bi = order[i];
        ud_frame_block_info_t *info = &report->blocks[i];
        info->index = bi;
        info->begin = graph->blocks[bi].begin;
        info->end = graph->blocks[bi].end;
        info->reachable = evaluated[bi] ? 1 : 0;
        if (evaluated[bi]) {
            info->entry_state = evaluated_states[bi];
            info->exit_state = out_states[bi];
        }
    }

    /* Publish instructions in stable block-start/offset order. */
    for (i = 0; i < n; ++i) {
        size_t bi = order[i];
        const frame_block_t *fb = &blocks[bi];
        ud_frame_state_t state;
        size_t j;

        if (!evaluated[bi])
            continue;
        state = evaluated_states[bi];

        for (j = 0; j < fb->insn_count; ++j) {
            const frame_insn_t *insn = &fb->insns[j];
            int stop = 0;
            int idx = add_insn(&build, insn, bi, graph->blocks[bi].begin,
                               &state);
            if (idx < 0)
                goto fail;
            if (apply_insn(insn, &state, bi, graph->blocks[bi].begin,
                           &build, (size_t)idx, &stop, error) != 0)
                goto fail;
        }
    }

    for (i = 0; i < report->insn_count; ++i) {
        if (report->access_count != 0)
            report->insns[i].accesses =
                report->accesses + report->insns[i].access_start;
    }

    result = report;
    report = NULL;
    goto cleanup;

nomem_early:
    set_error(error, UD_FRAME_ERR_NOMEM, UD_FRAME_NO_BLOCK, 0,
              "out of memory");
    goto fail;

fail:
    if (report != NULL)
        ud_frame_report_free(report);
    result = NULL;

cleanup:
    if (blocks != NULL) {
        for (i = 0; i < n; ++i)
            free(blocks[i].insns);
    }
    free(order);
    free(blocks);
    free(pred_count);
    free(pred_offset);
    free(pred_data);
    free(pred_cursor);
    free(entry_states);
    free(evaluated_states);
    free(out_states);
    free(evaluated);
    free(queued);
    free(stack);
    return result;
}

void
ud_frame_report_free(ud_frame_report_t *report)
{
    if (report == NULL)
        return;
    free(report->insns);
    free(report->accesses);
    free(report->blocks);
    free(report);
}

void
ud_frame_free(ud_frame_report_t *report)
{
    ud_frame_report_free(report);
}

void
ud_frame_analyze_free(ud_frame_report_t *report)
{
    ud_frame_report_free(report);
}
