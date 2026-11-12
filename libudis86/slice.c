/* udis86 - libudis86/slice.c
 *
 * Backward slicing of straight-line x86-64 code.  See libudis86/slice.h
 * for the public interface and the exact scope of the supported
 * instruction set.
 *
 * The implementation decodes the fragment once with the regular libudis86
 * decoder, rejects anything outside the supported set, and then walks the
 * instructions backwards, tracking liveness per register bit and per
 * flag.  The input code is never executed.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "types.h"
#include "extern.h"
#include "slice.h"

#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------------
 * Small bit-mask helpers.  All masks refer to bits of the canonical 64-bit
 * register identities.
 * -----------------------------------------------------------------------------
 */

/* bits [off, off+w) */
static uint64_t
rangemask(unsigned off, unsigned w)
{
  if (w == 0 || off >= 64) {
    return 0;
  }
  if (w >= 64 - off) {
    return ~0ull << off;
  }
  return ((1ull << w) - 1) << off;
}

/*
 * Bits of the canonical register killed by a write of width w at offset
 * off.  8 and 16 bit writes preserve the rest of the register; 32 bit
 * writes determine the upper half to zero, so they kill the whole
 * register.
 */
static uint64_t
writemask(unsigned off, unsigned w)
{
  if (w == 32) {
    return ~0ull;
  }
  return rangemask(off, w);
}

/* move bits [from_off, from_off+w) of a mask to [to_off, to_off+w) */
static uint64_t
map_bits(uint64_t bits, unsigned from_off, unsigned to_off, unsigned w)
{
  uint64_t r = 0;
  unsigned i;
  for (i = 0; i < w && from_off + i < 64 && to_off + i < 64; i++) {
    if ((bits >> (from_off + i)) & 1) {
      r |= 1ull << (to_off + i);
    }
  }
  return r;
}

/* index of the highest set bit of v (v must be non-zero) */
static unsigned
hi_bit(uint64_t v)
{
  unsigned h = 0;
  while (v >>= 1) {
    h++;
  }
  return h;
}

/* remove mask from live[g], returning the bits that were live */
static uint64_t
kill_bits(uint64_t live[UD_SLICE_NUM_REGS], int g, uint64_t mask)
{
  uint64_t rel = live[g] & mask;
  live[g] &= ~mask;
  return rel;
}

/*
 * Map a udis86 register to its canonical 64-bit identity.  Returns the
 * GPR index (0..15) and stores the bit offset and width of the alias, or
 * -1 if the register is not a general purpose register.
 */
static int
gpr_of(enum ud_type t, unsigned *off, unsigned *width)
{
  if (t >= UD_R_AL && t <= UD_R_R15B) {
    int i = t - UD_R_AL;
    *width = 8;
    if (i < 4) {              /* al..bl   */
      *off = 0;
      return i;
    } else if (i < 8) {       /* ah..bh   */
      *off = 8;
      return i - 4;
    } else {                  /* spl..r15b */
      *off = 0;
      return i - 4;
    }
  }
  if (t >= UD_R_AX && t <= UD_R_R15W) {
    *off = 0; *width = 16;
    return t - UD_R_AX;
  }
  if (t >= UD_R_EAX && t <= UD_R_R15D) {
    *off = 0; *width = 32;
    return t - UD_R_EAX;
  }
  if (t >= UD_R_RAX && t <= UD_R_R15) {
    *off = 0; *width = 64;
    return t - UD_R_RAX;
  }
  return -1;
}

/* immediate value, sign extended and masked to width w */
static uint64_t
imm_bits(const struct ud_operand *o, unsigned w)
{
  uint64_t v;
  switch (o->size) {
  case 8:  v = (uint64_t)(int64_t)o->lval.sbyte;  break;
  case 16: v = (uint64_t)(int64_t)o->lval.sword;  break;
  case 32: v = (uint64_t)(int64_t)o->lval.sdword; break;
  case 64: v = o->lval.uqword;                    break;
  default: v = 0;                                 break;
  }
  return v & rangemask(0, w);
}

/* -----------------------------------------------------------------------------
 * Instruction classification
 * -----------------------------------------------------------------------------
 */

static int
is_cmovcc(enum ud_mnemonic_code m)
{
  return m >= UD_Icmovo && m <= UD_Icmovg;
}

static int
is_setcc(enum ud_mnemonic_code m)
{
  return m >= UD_Iseto && m <= UD_Isetg;
}

/* flags a condition code really reads */
static uint8_t
cc_flags(enum ud_mnemonic_code m)
{
  switch (m) {
  case UD_Icmovo:  case UD_Iseto:
  case UD_Icmovno: case UD_Isetno:  return UD_SLICE_OF;
  case UD_Icmovb:  case UD_Isetb:
  case UD_Icmovae: case UD_Isetae:  return UD_SLICE_CF;
  case UD_Icmovz:  case UD_Isetz:
  case UD_Icmovnz: case UD_Isetnz:  return UD_SLICE_ZF;
  case UD_Icmovbe: case UD_Isetbe:
  case UD_Icmova:  case UD_Iseta:   return UD_SLICE_CF | UD_SLICE_ZF;
  case UD_Icmovs:  case UD_Isets:
  case UD_Icmovns: case UD_Isetns:  return UD_SLICE_SF;
  case UD_Icmovp:  case UD_Isetp:
  case UD_Icmovnp: case UD_Isetnp:  return UD_SLICE_PF;
  case UD_Icmovl:  case UD_Isetl:
  case UD_Icmovge: case UD_Isetge:  return UD_SLICE_SF | UD_SLICE_OF;
  case UD_Icmovle: case UD_Isetle:
  case UD_Icmovg:  case UD_Isetg:   return UD_SLICE_ZF | UD_SLICE_SF |
                                          UD_SLICE_OF;
  default:                          return 0;
  }
}

/* operand is a general purpose register */
static int
opr_is_gpr(const struct ud_operand *o)
{
  unsigned off, width;
  return o != NULL && o->type == UD_OP_REG &&
         gpr_of(o->base, &off, &width) >= 0;
}

/*
 * Whether the decoded instruction belongs to the supported set.  Anything
 * with a memory operand, a non-GPR register, or an unexpected operand
 * shape is rejected here.
 */
static int
insn_supported(const ud_t *u)
{
  const struct ud_operand *op0 = ud_insn_opr(u, 0);
  const struct ud_operand *op1 = ud_insn_opr(u, 1);
  const struct ud_operand *op2 = ud_insn_opr(u, 2);

  if (op2 != NULL) {
    return 0;
  }
  switch (u->mnemonic) {
  case UD_Inop:
    /* NOP never touches architectural state, memory operand or not */
    return 1;
  case UD_Imov:
    return opr_is_gpr(op0) &&
           (opr_is_gpr(op1) || (op1 != NULL && op1->type == UD_OP_IMM));
  case UD_Imovzx:
  case UD_Imovsx:
  case UD_Imovsxd:
  case UD_Ixchg:
    return opr_is_gpr(op0) && opr_is_gpr(op1);
  case UD_Ixor:
  case UD_Iand:
  case UD_Ior:
  case UD_Iadd:
  case UD_Isub:
  case UD_Icmp:
  case UD_Itest:
    return opr_is_gpr(op0) &&
           (opr_is_gpr(op1) || (op1 != NULL && op1->type == UD_OP_IMM));
  default:
    break;
  }
  if (is_cmovcc(u->mnemonic)) {
    return opr_is_gpr(op0) && opr_is_gpr(op1);
  }
  if (is_setcc(u->mnemonic)) {
    return opr_is_gpr(op0) && op1 == NULL;
  }
  return 0;
}

/* -----------------------------------------------------------------------------
 * Backward pass
 * -----------------------------------------------------------------------------
 */

struct bwd_state
{
  uint64_t live[UD_SLICE_NUM_REGS];
  uint8_t live_flags;
  int err;          /* UD_SLICE_OK or UD_SLICE_ERR_UNDEF_FLAG */
  uint8_t err_flag; /* the undefined flag that was depended on */
};

/*
 * Flag bookkeeping for an instruction that writes flags.  fw is the mask
 * of flags the instruction defines (as values or constants), fu the mask
 * of flags it leaves undefined.  Returns the relevant flag bits; the
 * written flags are removed from the live set.
 */
static uint8_t
flags_backward(struct bwd_state *st, uint8_t fw, uint8_t fu)
{
  uint8_t rel = st->live_flags & (fw | fu);
  if (st->live_flags & fu) {
    st->err = UD_SLICE_ERR_UNDEF_FLAG;
    st->err_flag = (uint8_t)(st->live_flags & fu);
  }
  st->live_flags &= (uint8_t)~(fw | fu);
  return rel & fw;
}

/*
 * Process one instruction of the backward slice.  Fills out with the
 * relevant bits the instruction writes and updates the live sets.
 */
static void
slice_step(struct bwd_state *st, const ud_t *u, struct ud_slice_insn *out)
{
  enum ud_mnemonic_code m = u->mnemonic;
  const struct ud_operand *op0 = ud_insn_opr(u, 0);
  const struct ud_operand *op1 = ud_insn_opr(u, 1);
  unsigned o0off = 0, o0w = 0, o1off = 0, o1w = 0;
  int g0 = -1, g1 = -1;

  if (op0 != NULL && op0->type == UD_OP_REG) {
    g0 = gpr_of(op0->base, &o0off, &o0w);
  }
  if (op1 != NULL && op1->type == UD_OP_REG) {
    g1 = gpr_of(op1->base, &o1off, &o1w);
  }

  if (m == UD_Inop) {
    return;
  }

  if (is_setcc(m)) {
    /* writes only the target byte, depending on the condition flags */
    uint64_t rel = kill_bits(st->live, g0, rangemask(o0off, 8));
    out->reg_bits[g0] |= rel;
    if (rel != 0) {
      st->live_flags |= cc_flags(m);
    }
    return;
  }

  if (is_cmovcc(m)) {
    /*
     * The result may come from the source or may keep the old
     * destination, and it depends on the condition flags.  A 32 bit
     * CMOV determines the upper half to zero whatever the condition
     * is, so only the low 32 bits carry dependencies.
     */
    uint64_t rel = kill_bits(st->live, g0, writemask(o0off, o0w));
    out->reg_bits[g0] |= rel;
    if (rel != 0) {
      uint64_t w = rel & rangemask(o0off, o0w);
      st->live[g1] |= map_bits(w, o0off, o1off, o0w);
      st->live[g0] |= w;
      st->live_flags |= cc_flags(m);
    }
    return;
  }

  switch (m) {
  case UD_Imov: {
    uint64_t rel = kill_bits(st->live, g0, writemask(o0off, o0w));
    out->reg_bits[g0] |= rel;
    if (g1 >= 0) {
      st->live[g1] |= map_bits(rel & rangemask(o0off, o0w),
                               o0off, o1off, o0w);
    }
    /* immediate form: a relevant constant write, no dependencies */
    return;
  }
  case UD_Imovzx: {
    /* low bits come from the source, the rest is determined zero */
    uint64_t rel = kill_bits(st->live, g0, writemask(o0off, o0w));
    out->reg_bits[g0] |= rel;
    st->live[g1] |= map_bits(rel & rangemask(0, o1w), 0, o1off, o1w);
    return;
  }
  case UD_Imovsx:
  case UD_Imovsxd: {
    /* the upper bits of the result depend only on the source sign bit */
    uint64_t rel = kill_bits(st->live, g0, writemask(o0off, o0w));
    out->reg_bits[g0] |= rel;
    st->live[g1] |= map_bits(rel & rangemask(0, o1w), 0, o1off, o1w);
    if (rel & rangemask(o1w, o0w - o1w)) {
      st->live[g1] |= 1ull << (o1off + o1w - 1);
    }
    return;
  }
  case UD_Ixchg: {
    uint64_t ra = kill_bits(st->live, g0, writemask(o0off, o0w));
    uint64_t rb = kill_bits(st->live, g1, writemask(o1off, o1w));
    out->reg_bits[g0] |= ra;
    out->reg_bits[g1] |= rb;
    st->live[g1] |= map_bits(ra & rangemask(o0off, o0w), o0off, o1off, o0w);
    st->live[g0] |= map_bits(rb & rangemask(o1off, o1w), o1off, o0off, o1w);
    return;
  }
  case UD_Iand:
  case UD_Ior:
  case UD_Ixor:
  case UD_Itest: {
    /*
     * Bitwise operations.  ddep/sdep are the positions of the result
     * that depend on the old destination / on the source.  With an
     * immediate, known zero and one bits of the immediate cut the
     * dependency: AND keeps only bits where the immediate has a one,
     * OR only bits where it has a zero, XOR always depends.  XOR of a
     * register with itself is a constant zero and depends on nothing.
     */
    int writes = (m != UD_Itest);
    uint64_t rel = 0, ddep, sdep = 0, pos;
    uint8_t relf;
    if (writes) {
      rel = kill_bits(st->live, g0, writemask(o0off, o0w));
      out->reg_bits[g0] |= rel;
    }
    if (g1 >= 0) {
      if (m == UD_Ixor && op0->base == op1->base) {
        ddep = 0;
        sdep = 0;
      } else {
        ddep = rangemask(o0off, o0w);
        sdep = rangemask(o1off, o1w);
      }
    } else {
      uint64_t imm = imm_bits(op1, o0w);
      uint64_t sel;
      if (m == UD_Iand || m == UD_Itest) {
        sel = imm;
      } else if (m == UD_Ior) {
        sel = ~imm & rangemask(0, o0w);
      } else {
        sel = rangemask(0, o0w);
      }
      ddep = sel << o0off;
      sdep = 0;
    }
    /* CF and OF are determined zero, AF is undefined */
    relf = flags_backward(st, UD_SLICE_CF | UD_SLICE_OF | UD_SLICE_SF |
                              UD_SLICE_ZF | UD_SLICE_PF,
                          UD_SLICE_AF);
    out->flag_bits |= relf;
    /* result positions the live flags depend on */
    pos = rel;
    if (relf & UD_SLICE_ZF) {
      pos |= rangemask(o0off, o0w);
    }
    if (relf & UD_SLICE_SF) {
      pos |= 1ull << (o0off + o0w - 1);
    }
    if (relf & UD_SLICE_PF) {
      pos |= rangemask(o0off, 8);
    }
    st->live[g0] |= pos & ddep;
    if (g1 >= 0) {
      st->live[g1] |= map_bits(pos, o0off, o1off, o0w) & sdep;
    }
    return;
  }
  case UD_Iadd:
  case UD_Isub: {
    /*
     * Result bit i depends on input bits 0..i; the arithmetic flags
     * conservatively depend on the whole operands.
     */
    uint64_t rel = kill_bits(st->live, g0, writemask(o0off, o0w));
    uint64_t relw = rel & rangemask(o0off, o0w);
    uint64_t cum = 0;
    uint8_t relf;
    out->reg_bits[g0] |= rel;
    if (relw != 0) {
      unsigned hi = hi_bit(relw);
      cum = rangemask(o0off, hi - o0off + 1);
    }
    relf = flags_backward(st, UD_SLICE_ALL_FLAGS, 0);
    out->flag_bits |= relf;
    if (relf != 0) {
      cum |= rangemask(o0off, o0w);
    }
    st->live[g0] |= cum;
    if (g1 >= 0) {
      st->live[g1] |= map_bits(cum, o0off, o1off, o0w);
    }
    return;
  }
  case UD_Icmp: {
    /* no register write; flags depend on the whole operands */
    uint8_t relf = flags_backward(st, UD_SLICE_ALL_FLAGS, 0);
    out->flag_bits |= relf;
    if (relf != 0) {
      uint64_t pos = rangemask(o0off, o0w);
      st->live[g0] |= pos;
      if (g1 >= 0) {
        st->live[g1] |= map_bits(pos, o0off, o1off, o0w);
      }
    }
    return;
  }
  default:
    /* unreachable: insn_supported() filtered everything else */
    return;
  }
}

/* -----------------------------------------------------------------------------
 * Public entry points
 * -----------------------------------------------------------------------------
 */

struct dec_insn
{
  ud_t u;
  uint64_t off;
};

int
ud_slice(const uint8_t *code, size_t code_size,
         const uint64_t reg_mask[UD_SLICE_NUM_REGS],
         uint8_t flag_mask,
         struct ud_slice **result)
{
  struct ud_slice *res;
  struct dec_insn *insns = NULL;
  struct ud_slice_insn *outs = NULL;
  size_t n = 0, cap = 0, i, count;
  ud_t u;
  struct bwd_state st;

  if (result == NULL) {
    return UD_SLICE_ERR_NOMEM;
  }
  *result = NULL;
  res = (struct ud_slice *)calloc(1, sizeof(*res));
  if (res == NULL) {
    return UD_SLICE_ERR_NOMEM;
  }
  *result = res;
  res->status = UD_SLICE_OK;

  /*
   * Pass 1: decode the whole fragment and reject anything outside the
   * supported set, whether or not it would end up in the slice.
   */
  ud_init(&u);
  ud_set_mode(&u, 64);
  ud_set_input_buffer(&u, code, code_size);
  while (ud_disassemble(&u)) {
    if (u.mnemonic == UD_Iinvalid) {
      /* truncated or undecodable instruction */
      res->status = UD_SLICE_ERR_DECODE;
      res->error_offset = ud_insn_off(&u);
      goto done;
    }
    if (!insn_supported(&u)) {
      res->status = UD_SLICE_ERR_UNSUPPORTED;
      res->error_offset = ud_insn_off(&u);
      goto done;
    }
    if (n == cap) {
      size_t ncap = cap == 0 ? 16 : cap * 2;
      struct dec_insn *ni =
        (struct dec_insn *)realloc(insns, ncap * sizeof(*ni));
      if (ni == NULL) {
        res->status = UD_SLICE_ERR_NOMEM;
        goto done;
      }
      insns = ni;
      cap = ncap;
    }
    insns[n].u = u;
    insns[n].off = ud_insn_off(&u);
    n++;
  }

  /* Pass 2: walk backwards, tracking live register bits and flags. */
  memset(&st, 0, sizeof(st));
  if (reg_mask != NULL) {
    memcpy(st.live, reg_mask, sizeof(st.live));
  }
  st.live_flags = flag_mask & UD_SLICE_ALL_FLAGS;

  if (n != 0) {
    outs = (struct ud_slice_insn *)calloc(n, sizeof(*outs));
    if (outs == NULL) {
      res->status = UD_SLICE_ERR_NOMEM;
      goto done;
    }
  }
  for (i = n; i-- > 0;) {
    outs[i].offset = insns[i].off;
    slice_step(&st, &insns[i].u, &outs[i]);
    if (st.err != UD_SLICE_OK) {
      res->status = st.err;
      res->error_offset = insns[i].off;
      res->error_flag = st.err_flag;
      goto done;
    }
  }

  /* Compact the relevant instructions, sorted by offset. */
  count = 0;
  for (i = 0; i < n; i++) {
    int g, relevant = outs[i].flag_bits != 0;
    for (g = 0; g < UD_SLICE_NUM_REGS && !relevant; g++) {
      relevant = outs[i].reg_bits[g] != 0;
    }
    if (relevant) {
      outs[count++] = outs[i];
    }
  }
  res->insns = outs;
  res->insn_count = count;
  outs = NULL;
  memcpy(res->entry_regs, st.live, sizeof(res->entry_regs));
  res->entry_flags = st.live_flags;

done:
  free(outs);
  free(insns);
  if (res->status != UD_SLICE_OK) {
    /* errors never return half a slice */
    free(res->insns);
    res->insns = NULL;
    res->insn_count = 0;
  }
  return res->status;
}

void
ud_slice_free(struct ud_slice *slice)
{
  if (slice != NULL) {
    free(slice->insns);
    free(slice);
  }
}
