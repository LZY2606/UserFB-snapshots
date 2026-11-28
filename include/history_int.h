/****************************************************************************
*
* Realmode X86 Emulator Library
*
* Copyright (c) 2007-2017 SUSE LINUX GmbH; Author: Steffen Winterfeldt
*
*  ========================================================================
*
*  Permission to use, copy, modify, distribute, and sell this software and
*  its documentation for any purpose is hereby granted without fee,
*  provided that the above copyright notice appear in all copies and that
*  both that copyright notice and this permission notice appear in
*  supporting documentation, and that the name of the authors not be used
*  in advertising or publicity pertaining to distribution of the software
*  without specific, written prior permission.  The authors makes no
*  representations about the suitability of this software for any purpose.
*  It is provided "as is" without express or implied warranty.
*
*  THE AUTHORS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
*  INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
*  EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
*  CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
*  USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
*  OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
*  PERFORMANCE OF THIS SOFTWARE.
*
*  ========================================================================
*
* Description:
*   Internal declarations of the reversible (history) debugger. Not part of
*   the public interface.
*
****************************************************************************/


#ifndef __X86EMU_HISTORY_INT_H
#define __X86EMU_HISTORY_INT_H

#include "x86emu.h"

/*
 * One recorded byte change. The linear address uniquely identifies the
 * location, so a byte touched several times within one step (alias pages or a
 * REP loop) keeps only its very first pre-image; the records of a step are
 * undone in reverse order but the first-touch rule guarantees every location
 * is restored to its true pre-instruction content.
 */
typedef struct {
  u32 addr;
  u8 data;
  u8 attr;
} hist_byte_t;

/*
 * A page whose backing store was created during a step. Undoing the step
 * dematerializes it again (a sparse page that did not exist before the step
 * must be gone afterwards). External x86emu_set_page mappings are restored in
 * place instead and never appear here.
 */
typedef struct {
  u32 page_addr;		/* base address of the 4k page */
} hist_newpage_t;

/*
 * A single reversible step: a fixed size CPU register pre-image plus the
 * sparse, dynamically grown list of memory/attribute byte changes and pages
 * materialized by this step.
 */
typedef struct hist_step_s {
  x86emu_regs_t regs;		/* CPU state before the instruction */
  u8 mem_invalid;		/* mem->invalid before the instruction */
  u8 halted;			/* result was HLT */
  hist_byte_t *bytes;		/* first-touch byte pre-images */
  unsigned nbytes;
  unsigned bytes_cap;
  hist_newpage_t *pages;	/* pages created by this step */
  unsigned npages;
  unsigned pages_cap;
  struct hist_step_s *next;
  struct hist_step_s *prev;
} hist_step_t;

/*
 * The reversible debugger state, hung off x86emu_mem_t.hist. Recording is only
 * active while 'recording' is set, i.e. between the begin and the commit/abort
 * of a step.
 */
typedef struct x86emu_hist_s {
  hist_step_t *head;		/* oldest recorded step */
  hist_step_t *tail;		/* newest recorded step */
  hist_step_t *cur;		/* step currently being recorded, or NULL */
  unsigned entries;
  unsigned long data_bytes;	/* retained data pre-image bytes */
  unsigned long attr_bytes;	/* retained attribute pre-image bytes */
  unsigned long page_events;
  int recording;
  int barrier;			/* a host-visible instruction was met */
  int memfault;			/* a memory permission check failed */
} x86emu_hist_t;

/* lifecycle (history.c) */
x86emu_hist_t *emu_hist_new(void);
x86emu_hist_t *emu_hist_free(x86emu_hist_t *hist);
void emu_hist_invalidate(x86emu_t *emu);
int emu_hist_active(x86emu_t *emu);

/* step transaction (history.c) */
void emu_hist_begin(x86emu_t *emu);
void emu_hist_commit(x86emu_t *emu, int halted);
void emu_hist_abort(x86emu_t *emu);
int emu_hist_undo(x86emu_t *emu);

/* memory change interception (mem.c) */
void emu_hist_record_byte(x86emu_mem_t *mem, u32 addr, u8 data, u8 attr);
void emu_hist_record_new_page(x86emu_mem_t *mem, u32 page_addr);
void emu_hist_restore_byte(x86emu_mem_t *mem, u32 addr, u8 data, u8 attr);
void emu_hist_dematerialize_page(x86emu_mem_t *mem, u32 page_addr);

/* non disturbing observer (history.c) */
unsigned emu_hist_peek(x86emu_mem_t *mem, unsigned addr, unsigned len,
  unsigned char *data, unsigned char *attr);

#endif /* __X86EMU_HISTORY_INT_H */
