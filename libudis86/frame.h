/* udis86 - libudis86/frame.h
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
 * Frame analysis on top of libudis86.
 *
 * Given a read-only buffer of x86-64 machine code and a caller-recovered
 * basic block graph, ud_frame_analyze() computes, for every instruction of
 * every reachable block, the state of RSP and RBP immediately before the
 * instruction executes, together with the locations the instruction
 * accesses on the stack.
 *
 * The analysis only interprets bytes: it never executes the code, never
 * reads host registers or memory, and never recovers control flow on its
 * own; every edge comes from the caller.
 *
 * States are expressed relative to the value S of RSP at the entry block.
 * Each of RSP and RBP is, at any point, either S plus a signed 64-bit
 * constant, or unknown.  At a join a constant survives only when every
 * reached predecessor agrees on it, otherwise the component is unknown.
 * The two components are tracked independently, so an unknown RSP never
 * drags a known RBP down with it.
 */
#ifndef UD_FRAME_H
#define UD_FRAME_H

#if !defined(__UD_STANDALONE__)
# include <stdint.h>
# include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LIBUDIS86_DLLEXTERN
# if defined(_MSC_VER) && defined(_USRDLL)
#  ifdef LIBUDIS86_EXPORTS
#   define LIBUDIS86_DLLEXTERN __declspec(dllexport)
#  else
#   define LIBUDIS86_DLLEXTERN __declspec(dllimport)
#  endif
# else
#  define LIBUDIS86_DLLEXTERN
# endif
#endif

/* -----------------------------------------------------------------------------
 * A single stack-frame value, relative to the entry RSP value S.
 * -----------------------------------------------------------------------------
 */
enum ud_frame_value_kind
{
  UD_FRAME_VALUE_UNREACHED = 0, /* point not reached by the analysis      */
  UD_FRAME_VALUE_UNKNOWN,       /* reached, but the value is unknown      */
  UD_FRAME_VALUE_OFFSET         /* value is S + offset                    */
};

struct ud_frame_value
{
  enum ud_frame_value_kind kind;
  int64_t offset; /* valid only when kind == UD_FRAME_VALUE_OFFSET */
};

/* -----------------------------------------------------------------------------
 * One recorded stack access of an instruction.
 * -----------------------------------------------------------------------------
 */
enum ud_frame_access_kind
{
  UD_FRAME_ACCESS_READ = 0, /* memory is read                        */
  UD_FRAME_ACCESS_WRITE,    /* memory is written                     */
  UD_FRAME_ACCESS_ADDR      /* address computation only (LEA)        */
};

/* operand index used for implicit stack slots (PUSH/POP/CALL/RET/LEAVE) */
#define UD_FRAME_ACCESS_IMPLICIT ((unsigned int) -1)

struct ud_frame_access
{
  enum ud_frame_access_kind kind;
  unsigned int operand;     /* explicit operand index, or
                               UD_FRAME_ACCESS_IMPLICIT for implicit slots */
  unsigned int size;        /* access width in bits; 0 for UD_FRAME_ACCESS_ADDR */
  struct ud_frame_value address; /* accessed address, relative to S; may be
                                    UD_FRAME_VALUE_UNKNOWN */
};

/* -----------------------------------------------------------------------------
 * Analysis results.  Everything below is owned by the report.
 * -----------------------------------------------------------------------------
 */
struct ud_frame_insn
{
  uint64_t offset;                /* offset of the instruction in the code */
  unsigned int size;              /* instruction length in bytes           */
  struct ud_frame_value rsp;      /* RSP state before the instruction      */
  struct ud_frame_value rbp;      /* RBP state before the instruction      */
  struct ud_frame_access *accesses;
  size_t access_count;
};

struct ud_frame_block
{
  size_t index;                   /* caller-supplied block index           */
  uint64_t begin;                 /* half-open range into the code buffer  */
  uint64_t end;
  struct ud_frame_insn *insns;    /* in decode (offset) order              */
  size_t insn_count;
};

struct ud_frame_report
{
  /* reachable blocks only, sorted by begin offset */
  struct ud_frame_block *blocks;
  size_t block_count;
};

/* -----------------------------------------------------------------------------
 * Input: the caller-recovered basic block graph.
 *
 * Blocks are half-open offset ranges [begin, end) into the code buffer.
 * Ranges must not overlap, every range must decode exactly to its end, and
 * every successor index must name an existing block.  Edges are entirely
 * caller supplied: both sides of a conditional branch, the fall-through
 * after a CALL (the call target is not followed), and so on.  Blocks that
 * cannot be reached from the entry produce no instruction results.
 * -----------------------------------------------------------------------------
 */
struct ud_frame_block_desc
{
  uint64_t begin;
  uint64_t end;
  const size_t *successors;
  size_t successor_count;
};

struct ud_frame_graph
{
  const uint8_t *code;
  size_t code_size;
  size_t entry; /* index of the unique entry block */
  const struct ud_frame_block_desc *blocks;
  size_t block_count;
};

/* -----------------------------------------------------------------------------
 * Status and error reporting.
 * -----------------------------------------------------------------------------
 */
enum ud_frame_status
{
  UD_FRAME_OK = 0,
  UD_FRAME_E_ARGUMENT,    /* malformed graph description (NULL pointers,
                             empty graph, entry out of range)             */
  UD_FRAME_E_RANGE,       /* bad block range: empty, out of bounds, or
                             overlapping another block                   */
  UD_FRAME_E_EDGE,        /* successor index out of range                */
  UD_FRAME_E_DECODE,      /* illegal or truncated instruction in a
                             reachable block                             */
  UD_FRAME_E_UNSUPPORTED, /* instruction outside the supported set in a
                             reachable block                             */
  UD_FRAME_E_MEMORY       /* allocation failure                          */
};

#define UD_FRAME_NO_BLOCK ((size_t) -1)

struct ud_frame_error
{
  enum ud_frame_status code;
  size_t block;    /* block involved, or UD_FRAME_NO_BLOCK */
  uint64_t offset; /* offset into the code buffer */
};

/* -----------------------------------------------------------------------------
 * ud_frame_analyze()
 *
 * Analyze the graph and return a newly allocated, independently owned
 * report in *report_out.  The graph and the code buffer are only read and
 * are not retained; the report stays valid after they are released.
 *
 * On success UD_FRAME_OK is returned and *report_out is set.  On any
 * failure *report_out is set to NULL, no partial report is published, and
 * when error is not NULL it is filled with the block and offset involved.
 * -----------------------------------------------------------------------------
 */
extern LIBUDIS86_DLLEXTERN enum ud_frame_status
ud_frame_analyze(const struct ud_frame_graph *graph,
                 struct ud_frame_report **report_out,
                 struct ud_frame_error *error);

/* -----------------------------------------------------------------------------
 * ud_frame_report_free()
 *
 * Release a report returned by ud_frame_analyze().  NULL is accepted.
 * -----------------------------------------------------------------------------
 */
extern LIBUDIS86_DLLEXTERN void
ud_frame_report_free(struct ud_frame_report *report);

#ifdef __cplusplus
}
#endif
#endif /* UD_FRAME_H */
