/* udis86 - libudis86/slice.h
 *
 * Optional backward slicing interface for straight-line x86-64 code.
 *
 * Given a read-only buffer of machine code and a set of general purpose
 * register bits and flags to track at the end of the fragment, ud_slice()
 * works backwards through the fragment and reports which instructions the
 * tracked values depend on, which bits each of those instructions writes,
 * and which register bits and flags are still live at the fragment entry.
 *
 * The input code is never executed; it is only decoded with the regular
 * libudis86 decoder.
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
#ifndef UD_SLICE_H
#define UD_SLICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical 64-bit identities of the general purpose registers.  Every
 * alias of a register (al/ah/ax/eax/rax, ...) maps to the same slot, and
 * bit i of a mask refers to bit i of the 64-bit register.
 */
enum ud_slice_reg
{
  UD_SLICE_RAX = 0,
  UD_SLICE_RCX,
  UD_SLICE_RDX,
  UD_SLICE_RBX,
  UD_SLICE_RSP,
  UD_SLICE_RBP,
  UD_SLICE_RSI,
  UD_SLICE_RDI,
  UD_SLICE_R8,
  UD_SLICE_R9,
  UD_SLICE_R10,
  UD_SLICE_R11,
  UD_SLICE_R12,
  UD_SLICE_R13,
  UD_SLICE_R14,
  UD_SLICE_R15,
  UD_SLICE_NUM_REGS
};

/* Tracked flag bits (a compact encoding, not the eflags layout). */
#define UD_SLICE_CF 0x01u
#define UD_SLICE_PF 0x02u
#define UD_SLICE_AF 0x04u
#define UD_SLICE_ZF 0x08u
#define UD_SLICE_SF 0x10u
#define UD_SLICE_OF 0x20u
#define UD_SLICE_ALL_FLAGS 0x3fu

/*
 * Status codes returned by ud_slice() and stored in ud_slice.status.
 */
enum ud_slice_status
{
  UD_SLICE_OK = 0,        /* slice computed successfully                  */
  UD_SLICE_ERR_DECODE,    /* truncated or undecodable instruction         */
  UD_SLICE_ERR_UNSUPPORTED, /* instruction outside the supported set      */
  UD_SLICE_ERR_UNDEF_FLAG,  /* a dependency reached an undefined flag     */
  UD_SLICE_ERR_NOMEM      /* allocation failure                           */
};

/*
 * One instruction of the computed slice.
 */
struct ud_slice_insn
{
  uint64_t offset;                        /* byte offset into the input   */
  uint64_t reg_bits[UD_SLICE_NUM_REGS];   /* relevant bits written to
                                             each GPR                    */
  uint8_t  flag_bits;                     /* relevant flags written
                                             (UD_SLICE_* mask)           */
};

/*
 * Result of ud_slice().  On error, insn_count is 0 and insns is NULL;
 * error_offset (and error_flag for UD_SLICE_ERR_UNDEF_FLAG) describe the
 * offending instruction.
 */
struct ud_slice
{
  int status;                             /* enum ud_slice_status         */
  uint64_t error_offset;                  /* offset of the instruction
                                             that caused the error       */
  uint8_t error_flag;                     /* UD_SLICE_* bit, for
                                             UD_SLICE_ERR_UNDEF_FLAG     */
  size_t insn_count;                      /* number of sliced instructions*/
  struct ud_slice_insn *insns;            /* slice, sorted by offset      */
  uint64_t entry_regs[UD_SLICE_NUM_REGS]; /* register bits still coming
                                             from the fragment entry     */
  uint8_t entry_flags;                    /* flags still coming from the
                                             fragment entry              */
};

/*
 * ud_slice() - Compute a backward slice of a straight-line x86-64 code
 * fragment.
 *
 *   code/code_size - read-only machine code, decoded in 64-bit mode.
 *   reg_mask       - per-GPR bit masks to track at the end of the
 *                    fragment (may be NULL, meaning no registers).
 *   flag_mask      - UD_SLICE_* flags to track at the end of the fragment.
 *   result         - receives a newly allocated result, released with
 *                    ud_slice_free().  Set only if the return value is
 *                    not UD_SLICE_ERR_NOMEM.
 *
 * Returns UD_SLICE_OK on success (also stored in result->status).
 *
 * Supported instructions: MOV, MOVZX, MOVSX, MOVSXD, XCHG, XOR, AND, OR,
 * ADD, SUB, CMP and TEST in register or immediate form, CMOVcc and SETcc
 * in register form, and NOP.  Anything else (branches, calls, returns,
 * memory operands, ...) fails with UD_SLICE_ERR_UNSUPPORTED, whether or
 * not the instruction would have been part of the slice.
 */
extern int ud_slice(const uint8_t *code, size_t code_size,
                    const uint64_t reg_mask[UD_SLICE_NUM_REGS],
                    uint8_t flag_mask,
                    struct ud_slice **result);

/*
 * ud_slice_free() - Release a result returned by ud_slice().  NULL is
 * accepted and does nothing.
 */
extern void ud_slice_free(struct ud_slice *slice);

#ifdef __cplusplus
}
#endif

#endif /* UD_SLICE_H */
