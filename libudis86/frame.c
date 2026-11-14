/* udis86 - libudis86/frame.c
 *
 * Copyright (c) 2002-2013 Vivek Thampi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice,
 *       this list of conditions and the following disclaimer in the documentation
 *       and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Frame analysis: per-instruction RSP/RBP state relative to the entry
 * stack pointer S, plus the stack locations each instruction touches.
 * See frame.h for the model.  Only bytes are interpreted; control flow
 * comes entirely from the caller-supplied graph.
 */

#include "frame.h"
#include "types.h"
#include "extern.h"

#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Internal value/state machinery
 * =============================================================================
 */

typedef struct ud_frame_value fvalue_t;

typedef struct {
  struct ud_frame_value rsp;
  struct ud_frame_value rbp;
} fstate_t;

static fvalue_t
fvalue_unknown(void)
{
  fvalue_t v;
  v.kind = UD_FRAME_VALUE_UNKNOWN;
  v.offset = 0;
  return v;
}

static fvalue_t
fvalue_unreached(void)
{
  fvalue_t v;
  v.kind = UD_FRAME_VALUE_UNREACHED;
  v.offset = 0;
  return v;
}

static fvalue_t
fvalue_offset(int64_t off)
{
  fvalue_t v;
  v.kind = UD_FRAME_VALUE_OFFSET;
  v.offset = off;
  return v;
}

/* v + d, saturating to unknown on int64 overflow */
static fvalue_t
fvalue_add(fvalue_t v, int64_t d)
{
  if (v.kind != UD_FRAME_VALUE_OFFSET)
    return v;
  if ((d > 0 && v.offset > INT64_MAX - d) ||
      (d < 0 && v.offset < INT64_MIN - d))
    return fvalue_unknown();
  v.offset += d;
  return v;
}

static int
fvalue_eq(fvalue_t a, fvalue_t b)
{
  return a.kind == b.kind &&
         (a.kind != UD_FRAME_VALUE_OFFSET || a.offset == b.offset);
}

/*
 * Join: a constant survives only if both sides agree on it.  Unreached
 * is the identity, anything mixed with unknown is unknown.
 */
static fvalue_t
fvalue_merge(fvalue_t a, fvalue_t b)
{
  if (a.kind == UD_FRAME_VALUE_UNREACHED)
    return b;
  if (b.kind == UD_FRAME_VALUE_UNREACHED)
    return a;
  if (a.kind == UD_FRAME_VALUE_UNKNOWN || b.kind == UD_FRAME_VALUE_UNKNOWN)
    return fvalue_unknown();
  return a.offset == b.offset ? a : fvalue_unknown();
}

static fstate_t
fstate_unreached(void)
{
  fstate_t s;
  s.rsp = fvalue_unreached();
  s.rbp = fvalue_unreached();
  return s;
}

static fstate_t
fstate_merge(fstate_t a, fstate_t b)
{
  fstate_t s;
  s.rsp = fvalue_merge(a.rsp, b.rsp);
  s.rbp = fvalue_merge(a.rbp, b.rbp);
  return s;
}

static int
fstate_eq(fstate_t a, fstate_t b)
{
  return fvalue_eq(a.rsp, b.rsp) && fvalue_eq(a.rbp, b.rbp);
}

/* tracked register components */
enum { TR_NONE = 0, TR_RSP, TR_RBP };

static int
tracked_reg(enum ud_type t)
{
  switch (t) {
  case UD_R_RSP: case UD_R_ESP: case UD_R_SP: case UD_R_SPL:
    return TR_RSP;
  case UD_R_RBP: case UD_R_EBP: case UD_R_BP: case UD_R_BPL:
    return TR_RBP;
  default:
    return TR_NONE;
  }
}

static int
is_full64(enum ud_type t)
{
  return t == UD_R_RSP || t == UD_R_RBP;
}

static fvalue_t
comp_get(const fstate_t *st, int tr)
{
  return tr == TR_RSP ? st->rsp : st->rbp;
}

static void
comp_set(fstate_t *st, int tr, fvalue_t v)
{
  if (tr == TR_RSP)
    st->rsp = v;
  else
    st->rbp = v;
}

/* =============================================================================
 * Decoded instruction snapshot
 * =============================================================================
 */

typedef struct {
  uint64_t off;                 /* offset into the code buffer */
  unsigned int len;
  enum ud_mnemonic_code mn;
  struct ud_operand opr[2];
  uint8_t pfx_adr;
  uint8_t pfx_seg;
  uint8_t pfx_opr;
} fdinsn_t;

typedef struct {
  uint64_t begin, end;
  int reachable;
  fdinsn_t *insns;
  size_t insn_count, insn_cap;
  size_t *preds;
  size_t pred_count;
  fstate_t in, out;
} fblock_t;

typedef struct {
  const uint8_t *code;
  size_t code_size;
  size_t entry;
  size_t block_count;
  fblock_t *blocks;
} fctx_t;

/* =============================================================================
 * Operand helpers
 * =============================================================================
 */

static int64_t
opr_disp(const struct ud_operand *o)
{
  switch (o->offset) {
  case 8:  return o->lval.sbyte;
  case 16: return o->lval.sword;
  case 32: return o->lval.sdword;
  case 64: return o->lval.sqword;
  default: return 0;
  }
}

static int64_t
opr_imm(const struct ud_operand *o)
{
  switch (o->size) {
  case 8:  return o->lval.sbyte;
  case 16: return o->lval.sword;
  case 32: return o->lval.sdword;
  case 64: return o->lval.sqword;
  default: return 0;
  }
}

/*
 * Effective address of a memory operand, relative to S.  Unknown when the
 * base is not a tracked register, when an index register is involved, on
 * an address-size override, or under an FS/GS segment override.
 */
static fvalue_t
mem_addr(const fdinsn_t *in, const struct ud_operand *o, const fstate_t *st)
{
  int tr;
  if (in->pfx_adr)
    return fvalue_unknown();
  if (in->pfx_seg == UD_R_FS || in->pfx_seg == UD_R_GS)
    return fvalue_unknown();
  if (o->index != UD_NONE)
    return fvalue_unknown();
  tr = tracked_reg(o->base);
  if (tr == TR_NONE)
    return fvalue_unknown();
  return fvalue_add(comp_get(st, tr), opr_disp(o));
}

/* =============================================================================
 * Instruction support check and transfer
 * =============================================================================
 */

static int
is_jcc(enum ud_mnemonic_code mn)
{
  switch (mn) {
  case UD_Ija:  case UD_Ijae: case UD_Ijb:  case UD_Ijbe:
  case UD_Ijg:  case UD_Ijge: case UD_Ijl:  case UD_Ijle:
  case UD_Ijno: case UD_Ijnp: case UD_Ijns: case UD_Ijnz:
  case UD_Ijo:  case UD_Ijp:  case UD_Ijs:  case UD_Ijz:
  case UD_Ijcxz: case UD_Ijecxz: case UD_Ijrcxz:
    return 1;
  default:
    return 0;
  }
}

static int
insn_supported(const fdinsn_t *in)
{
  switch (in->mn) {
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
    return 1;
  case UD_Ipush:
    /* 64-bit register or immediate pushes only */
    if (in->pfx_opr)
      return 0;
    if (in->opr[0].type == UD_OP_IMM)
      return 1;
    return in->opr[0].type == UD_OP_REG && in->opr[0].size == 64;
  case UD_Ipop:
    /* 64-bit register pops only */
    return !in->pfx_opr &&
           in->opr[0].type == UD_OP_REG && in->opr[0].size == 64;
  default:
    return is_jcc(in->mn);
  }
}

static void
emit_access(struct ud_frame_access *acc, int *nacc,
            enum ud_frame_access_kind kind, unsigned int operand,
            unsigned int size, fvalue_t address)
{
  acc[*nacc].kind = kind;
  acc[*nacc].operand = operand;
  acc[*nacc].size = size;
  acc[*nacc].address = address;
  ++*nacc;
}

/*
 * Apply one instruction to the state.  acc must have room for 2 entries;
 * the number recorded is returned in *nacc.  Addresses of implicit slots
 * use the state as it was before the instruction.
 */
static fstate_t
insn_transfer(const fdinsn_t *in, fstate_t st,
              struct ud_frame_access *acc, int *nacc)
{
  int na = 0;
  const struct ud_operand *d = &in->opr[0];
  const struct ud_operand *s = &in->opr[1];

  switch (in->mn) {
  case UD_Inop:
    break;

  case UD_Imov:
    if (d->type == UD_OP_MEM) {
      emit_access(acc, &na, UD_FRAME_ACCESS_WRITE, 0, d->size,
                  mem_addr(in, d, &st));
    } else if (d->type == UD_OP_REG) {
      int tr = tracked_reg(d->base);
      if (s->type == UD_OP_MEM) {
        emit_access(acc, &na, UD_FRAME_ACCESS_READ, 1, s->size,
                    mem_addr(in, s, &st));
        if (tr != TR_NONE)
          comp_set(&st, tr, fvalue_unknown());
      } else if (tr != TR_NONE) {
        /* register or immediate source */
        if (is_full64(d->base) && s->type == UD_OP_REG &&
            is_full64(s->base) && tracked_reg(s->base) != TR_NONE)
          comp_set(&st, tr, comp_get(&st, tracked_reg(s->base)));
        else
          /* partial alias writes and plain constants break the relation */
          comp_set(&st, tr, fvalue_unknown());
      }
    }
    break;

  case UD_Ilea: {
    fvalue_t a = mem_addr(in, s, &st);
    int tr;
    emit_access(acc, &na, UD_FRAME_ACCESS_ADDR, 1, 0, a);
    tr = tracked_reg(d->base);
    if (tr != TR_NONE)
      comp_set(&st, tr, is_full64(d->base) ? a : fvalue_unknown());
    break;
  }

  case UD_Iadd:
  case UD_Isub:
    if (d->type == UD_OP_REG) {
      int tr = tracked_reg(d->base);
      if (tr != TR_NONE) {
        if (is_full64(d->base) && s->type == UD_OP_IMM) {
          int64_t v = opr_imm(s);
          if (in->mn == UD_Isub)
            v = -v;
          comp_set(&st, tr, fvalue_add(comp_get(&st, tr), v));
        } else {
          comp_set(&st, tr, fvalue_unknown());
        }
      }
    }
    break;

  case UD_Iand:
    /* any AND writing RSP/RBP (or an alias) destroys the relation */
    if (d->type == UD_OP_REG) {
      int tr = tracked_reg(d->base);
      if (tr != TR_NONE)
        comp_set(&st, tr, fvalue_unknown());
    }
    break;

  case UD_Icmp:
  case UD_Itest:
    break;

  case UD_Icall:
    /* return address pushed at old RSP - 8; normal return convention
       keeps the net stack change at zero and preserves RBP */
    emit_access(acc, &na, UD_FRAME_ACCESS_WRITE, UD_FRAME_ACCESS_IMPLICIT,
                64, fvalue_add(st.rsp, -8));
    break;

  case UD_Iret: {
    int64_t delta = 8;
    emit_access(acc, &na, UD_FRAME_ACCESS_READ, UD_FRAME_ACCESS_IMPLICIT,
                64, st.rsp);
    if (d->type == UD_OP_IMM)
      delta += opr_imm(d);
    st.rsp = fvalue_add(st.rsp, delta);
    break;
  }

  case UD_Ileave:
    /* mov rsp, rbp; pop rbp -- the slot read is the old RBP */
    emit_access(acc, &na, UD_FRAME_ACCESS_READ, UD_FRAME_ACCESS_IMPLICIT,
                64, st.rbp);
    st.rsp = st.rbp;
    st.rbp = fvalue_unknown();
    break;

  case UD_Ipush:
    emit_access(acc, &na, UD_FRAME_ACCESS_WRITE, UD_FRAME_ACCESS_IMPLICIT,
                64, fvalue_add(st.rsp, -8));
    st.rsp = fvalue_add(st.rsp, -8);
    break;

  case UD_Ipop:
    emit_access(acc, &na, UD_FRAME_ACCESS_READ, UD_FRAME_ACCESS_IMPLICIT,
                64, st.rsp);
    if (d->base == UD_R_RSP) {
      /* POP RSP loads the value from memory; it is not just RSP + 8 */
      st.rsp = fvalue_unknown();
    } else {
      st.rsp = fvalue_add(st.rsp, 8);
      if (tracked_reg(d->base) == TR_RBP)
        st.rbp = fvalue_unknown();
    }
    break;

  default:
    /* UD_Ijmp and the conditional branches leave the state alone;
       everything else was rejected by insn_supported() */
    break;
  }

  *nacc = na;
  return st;
}

/*
 * Run a whole block.  When recs is not NULL, per-instruction records are
 * filled in (access arrays are allocated and owned by the caller).
 * Returns the block out-state; both components are unreached when the
 * block contains a RET, which does not propagate to successors.
 */
static fstate_t
block_transfer(const fblock_t *b, fstate_t st, struct ud_frame_insn *recs,
               int *oom)
{
  size_t i;
  int diverged = 0;
  for (i = 0; i < b->insn_count; ++i) {
    const fdinsn_t *in = &b->insns[i];
    struct ud_frame_access tmp[2];
    int na = 0;
    if (recs) {
      recs[i].offset = in->off;
      recs[i].size = in->len;
      recs[i].rsp = st.rsp;
      recs[i].rbp = st.rbp;
      recs[i].accesses = NULL;
      recs[i].access_count = 0;
    }
    st = insn_transfer(in, st, tmp, &na);
    if (recs && na > 0) {
      recs[i].accesses = (struct ud_frame_access *)
                         malloc((size_t) na * sizeof(*recs[i].accesses));
      if (!recs[i].accesses) {
        *oom = 1;
        return fstate_unreached();
      }
      memcpy(recs[i].accesses, tmp, (size_t) na * sizeof(*tmp));
      recs[i].access_count = (size_t) na;
    }
    if (in->mn == UD_Iret)
      diverged = 1;
  }
  return diverged ? fstate_unreached() : st;
}

/* =============================================================================
 * Graph validation and decoding
 * =============================================================================
 */

static enum ud_frame_status
fail(struct ud_frame_error *err, enum ud_frame_status code,
     size_t block, uint64_t offset)
{
  if (err) {
    err->code = code;
    err->block = block;
    err->offset = offset;
  }
  return code;
}

typedef struct {
  uint64_t begin, end;
  size_t index;
} frange_t;

static int
frange_cmp(const void *pa, const void *pb)
{
  const frange_t *a = (const frange_t *) pa;
  const frange_t *b = (const frange_t *) pb;
  if (a->begin != b->begin)
    return a->begin < b->begin ? -1 : 1;
  return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

static enum ud_frame_status
validate_ranges(fctx_t *ctx, const struct ud_frame_graph *g,
                struct ud_frame_error *err)
{
  size_t i;
  frange_t *sorted;
  for (i = 0; i < g->block_count; ++i) {
    const struct ud_frame_block_desc *d = &g->blocks[i];
    if (d->begin >= d->end || d->end > g->code_size)
      return fail(err, UD_FRAME_E_RANGE, i, d->begin);
    ctx->blocks[i].begin = d->begin;
    ctx->blocks[i].end = d->end;
  }
  /* overlap check on ranges sorted by begin: an overlap shows up as a
     range that starts before the previous range ends */
  sorted = (frange_t *) malloc(g->block_count * sizeof(frange_t));
  if (!sorted)
    return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
  for (i = 0; i < g->block_count; ++i) {
    sorted[i].begin = ctx->blocks[i].begin;
    sorted[i].end = ctx->blocks[i].end;
    sorted[i].index = i;
  }
  qsort(sorted, g->block_count, sizeof(frange_t), frange_cmp);
  for (i = 1; i < g->block_count; ++i) {
    if (sorted[i].begin < sorted[i - 1].end) {
      enum ud_frame_status st =
        fail(err, UD_FRAME_E_RANGE, sorted[i].index, sorted[i].begin);
      free(sorted);
      return st;
    }
  }
  free(sorted);
  return UD_FRAME_OK;
}

static enum ud_frame_status
validate_edges(fctx_t *ctx, const struct ud_frame_graph *g,
               struct ud_frame_error *err)
{
  size_t i, j;
  for (i = 0; i < g->block_count; ++i) {
    const struct ud_frame_block_desc *d = &g->blocks[i];
    if (d->successor_count && !d->successors)
      return fail(err, UD_FRAME_E_ARGUMENT, i, d->begin);
    for (j = 0; j < d->successor_count; ++j)
      if (d->successors[j] >= g->block_count)
        return fail(err, UD_FRAME_E_EDGE, i, ctx->blocks[i].begin);
  }
  return UD_FRAME_OK;
}

static void
mark_reachable(fctx_t *ctx, const struct ud_frame_graph *g)
{
  size_t *stack;
  size_t sp = 0, i;
  stack = (size_t *) malloc(g->block_count * sizeof(size_t));
  if (!stack) {
    /* fall back to a slow O(n^2) closure without extra memory */
    int changed = 1;
    ctx->blocks[g->entry].reachable = 1;
    while (changed) {
      changed = 0;
      for (i = 0; i < g->block_count; ++i) {
        size_t j;
        if (!ctx->blocks[i].reachable)
          continue;
        for (j = 0; j < g->blocks[i].successor_count; ++j) {
          size_t s = g->blocks[i].successors[j];
          if (!ctx->blocks[s].reachable) {
            ctx->blocks[s].reachable = 1;
            changed = 1;
          }
        }
      }
    }
    return;
  }
  ctx->blocks[g->entry].reachable = 1;
  stack[sp++] = g->entry;
  while (sp) {
    size_t b = stack[--sp], j;
    for (j = 0; j < g->blocks[b].successor_count; ++j) {
      size_t s = g->blocks[b].successors[j];
      if (!ctx->blocks[s].reachable) {
        ctx->blocks[s].reachable = 1;
        stack[sp++] = s;
      }
    }
  }
  free(stack);
}

static enum ud_frame_status
decode_block(fctx_t *ctx, size_t bi, struct ud_frame_error *err)
{
  fblock_t *b = &ctx->blocks[bi];
  size_t len = (size_t) (b->end - b->begin);
  size_t pos = 0;
  ud_t u;

  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_input_buffer(&u, ctx->code + b->begin, len);

  while (pos < len) {
    unsigned int n = ud_decode(&u);
    fdinsn_t *in;
    if (n == 0 || u.mnemonic == UD_Iinvalid)
      return fail(err, UD_FRAME_E_DECODE, bi, b->begin + pos);
    if (b->insn_count == b->insn_cap) {
      size_t ncap = b->insn_cap ? b->insn_cap * 2 : 8;
      fdinsn_t *ni = (fdinsn_t *) realloc(b->insns, ncap * sizeof(*ni));
      if (!ni)
        return fail(err, UD_FRAME_E_MEMORY, bi, b->begin + pos);
      b->insns = ni;
      b->insn_cap = ncap;
    }
    in = &b->insns[b->insn_count++];
    in->off = b->begin + pos;
    in->len = n;
    in->mn = u.mnemonic;
    in->opr[0] = u.operand[0];
    in->opr[1] = u.operand[1];
    in->pfx_adr = u.pfx_adr;
    in->pfx_seg = u.pfx_seg;
    in->pfx_opr = u.pfx_opr;
    if (!insn_supported(in))
      return fail(err, UD_FRAME_E_UNSUPPORTED, bi, in->off);
    pos += n;
  }
  return UD_FRAME_OK;
}

/* =============================================================================
 * Dataflow
 * =============================================================================
 */

static enum ud_frame_status
build_preds(fctx_t *ctx, const struct ud_frame_graph *g,
            struct ud_frame_error *err)
{
  size_t i, j;
  size_t *fill;
  fill = (size_t *) calloc(g->block_count, sizeof(size_t));
  if (!fill)
    return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
  for (i = 0; i < g->block_count; ++i) {
    if (!ctx->blocks[i].reachable)
      continue;
    for (j = 0; j < g->blocks[i].successor_count; ++j)
      ctx->blocks[g->blocks[i].successors[j]].pred_count++;
  }
  for (i = 0; i < g->block_count; ++i) {
    if (!ctx->blocks[i].pred_count)
      continue;
    ctx->blocks[i].preds =
      (size_t *) malloc(ctx->blocks[i].pred_count * sizeof(size_t));
    if (!ctx->blocks[i].preds) {
      free(fill);
      return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
    }
  }
  for (i = 0; i < g->block_count; ++i) {
    if (!ctx->blocks[i].reachable)
      continue;
    for (j = 0; j < g->blocks[i].successor_count; ++j) {
      size_t s = g->blocks[i].successors[j];
      ctx->blocks[s].preds[fill[s]++] = i;
    }
  }
  free(fill);
  return UD_FRAME_OK;
}

/*
 * Iterate to a fixpoint.  A component only ever moves unreached -> known
 * -> unknown, so this terminates; back edges keep repropagating until no
 * block input changes, never keeping a first-found value.
 */
static void
propagate(fctx_t *ctx)
{
  fstate_t init;
  int changed = 1;
  init.rsp = fvalue_offset(0);      /* RSP == S at the entry */
  init.rbp = fvalue_unknown();      /* RBP unknown at the entry */
  while (changed) {
    size_t i;
    changed = 0;
    for (i = 0; i < ctx->block_count; ++i) {
      fblock_t *b = &ctx->blocks[i];
      fstate_t in;
      size_t j;
      if (!b->reachable)
        continue;
      in = (i == ctx->entry) ? init : fstate_unreached();
      for (j = 0; j < b->pred_count; ++j) {
        const fblock_t *p = &ctx->blocks[b->preds[j]];
        if (p->reachable)
          in = fstate_merge(in, p->out);
      }
      if (!fstate_eq(in, b->in)) {
        int oom = 0;
        b->in = in;
        b->out = block_transfer(b, in, NULL, &oom);
        changed = 1;
      }
    }
  }
}

/* =============================================================================
 * Report assembly
 * =============================================================================
 */

typedef struct {
  uint64_t begin;
  size_t index;
} border_t;

static int
border_cmp(const void *pa, const void *pb)
{
  const border_t *a = (const border_t *) pa;
  const border_t *b = (const border_t *) pb;
  if (a->begin != b->begin)
    return a->begin < b->begin ? -1 : 1;
  return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

void
ud_frame_report_free(struct ud_frame_report *report)
{
  size_t i, j;
  if (!report)
    return;
  for (i = 0; i < report->block_count; ++i) {
    struct ud_frame_block *b = &report->blocks[i];
    for (j = 0; j < b->insn_count; ++j)
      free(b->insns[j].accesses);
    free(b->insns);
  }
  free(report->blocks);
  free(report);
}

static enum ud_frame_status
build_report(fctx_t *ctx, const struct ud_frame_graph *g,
             struct ud_frame_report **report_out,
             struct ud_frame_error *err)
{
  struct ud_frame_report *rep;
  border_t *order;
  size_t i, nreach = 0;

  for (i = 0; i < g->block_count; ++i)
    if (ctx->blocks[i].reachable)
      ++nreach;

  rep = (struct ud_frame_report *) calloc(1, sizeof(*rep));
  order = (border_t *) malloc((nreach ? nreach : 1) * sizeof(*order));
  if (!rep || !order) {
    free(order);
    free(rep);
    return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
  }
  nreach = 0;
  for (i = 0; i < g->block_count; ++i)
    if (ctx->blocks[i].reachable) {
      order[nreach].begin = ctx->blocks[i].begin;
      order[nreach].index = i;
      ++nreach;
    }
  qsort(order, nreach, sizeof(*order), border_cmp);

  rep->blocks = (struct ud_frame_block *)
                calloc(nreach ? nreach : 1, sizeof(*rep->blocks));
  if (!rep->blocks) {
    free(order);
    free(rep);
    return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
  }
  rep->block_count = nreach;

  for (i = 0; i < nreach; ++i) {
    fblock_t *b = &ctx->blocks[order[i].index];
    struct ud_frame_block *rb = &rep->blocks[i];
    int oom = 0;
    rb->index = order[i].index;
    rb->begin = b->begin;
    rb->end = b->end;
    rb->insn_count = b->insn_count;
    rb->insns = (struct ud_frame_insn *)
                calloc(b->insn_count ? b->insn_count : 1, sizeof(*rb->insns));
    if (!rb->insns) {
      free(order);
      ud_frame_report_free(rep);
      return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
    }
    block_transfer(b, b->in, rb->insns, &oom);
    if (oom) {
      free(order);
      ud_frame_report_free(rep);
      return fail(err, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
    }
  }
  free(order);
  *report_out = rep;
  return UD_FRAME_OK;
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

enum ud_frame_status
ud_frame_analyze(const struct ud_frame_graph *graph,
                 struct ud_frame_report **report_out,
                 struct ud_frame_error *error)
{
  fctx_t ctx;
  enum ud_frame_status st;
  size_t i;

  if (report_out)
    *report_out = NULL;
  if (!graph || !report_out || !graph->code || !graph->blocks ||
      !graph->block_count || graph->entry >= graph->block_count)
    return fail(error, UD_FRAME_E_ARGUMENT, UD_FRAME_NO_BLOCK, 0);

  memset(&ctx, 0, sizeof(ctx));
  ctx.code = graph->code;
  ctx.code_size = graph->code_size;
  ctx.entry = graph->entry;
  ctx.block_count = graph->block_count;
  ctx.blocks = (fblock_t *) calloc(graph->block_count, sizeof(*ctx.blocks));
  if (!ctx.blocks)
    return fail(error, UD_FRAME_E_MEMORY, UD_FRAME_NO_BLOCK, 0);
  for (i = 0; i < ctx.block_count; ++i) {
    ctx.blocks[i].in = fstate_unreached();
    ctx.blocks[i].out = fstate_unreached();
  }

  st = validate_ranges(&ctx, graph, error);
  if (st == UD_FRAME_OK)
    st = validate_edges(&ctx, graph, error);
  if (st == UD_FRAME_OK) {
    mark_reachable(&ctx, graph);
    for (i = 0; i < ctx.block_count && st == UD_FRAME_OK; ++i)
      if (ctx.blocks[i].reachable)
        st = decode_block(&ctx, i, error);
  }
  if (st == UD_FRAME_OK)
    st = build_preds(&ctx, graph, error);
  if (st == UD_FRAME_OK) {
    propagate(&ctx);
    st = build_report(&ctx, graph, report_out, error);
  }

  for (i = 0; i < ctx.block_count; ++i) {
    free(ctx.blocks[i].insns);
    free(ctx.blocks[i].preds);
  }
  free(ctx.blocks);
  return st;
}
