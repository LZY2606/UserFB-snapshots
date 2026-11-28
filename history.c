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
*   Reversible debugger. Records change records (never whole emulator clones
*   or whole pages) so that a single instruction, including a whole REP string
*   operation, can be undone.
*
****************************************************************************/


#include "include/x86emu_int.h"
#include "include/history_int.h"

/* exec one instruction, shared with the normal run loop primitives */
int emu_hist_exec_one(x86emu_t *emu);


x86emu_hist_t *emu_hist_new(void)
{
  return calloc(1, sizeof(x86emu_hist_t));
}


static void free_step(hist_step_t *step)
{
  if(!step) return;
  free(step->bytes);
  free(step->pages);
  free(step);
}


x86emu_hist_t *emu_hist_free(x86emu_hist_t *hist)
{
  hist_step_t *next;

  if(!hist) return NULL;

  while(hist->head) {
    next = hist->head->next;
    free_step(hist->head);
    hist->head = next;
  }

  free(hist);

  return NULL;
}


int emu_hist_active(x86emu_t *emu)
{
  return emu && emu->mem && emu->mem->hist;
}


/*
 * Drop every recorded step but keep the debugger session. Used when the guest
 * state is changed through a path the change recorder cannot follow (official
 * setters, reset, normal run, public memory access, access stat reset).
 */
void emu_hist_invalidate(x86emu_t *emu)
{
  x86emu_hist_t *hist;
  hist_step_t *next;

  if(!emu_hist_active(emu)) return;

  hist = emu->mem->hist;

  while(hist->head) {
    next = hist->head->next;
    free_step(hist->head);
    hist->head = next;
  }

  hist->head = hist->tail = hist->cur = NULL;
  hist->entries = 0;
  hist->data_bytes = hist->attr_bytes = hist->page_events = 0;
  hist->recording = 0;
}


void emu_hist_begin(x86emu_t *emu)
{
  x86emu_hist_t *hist = emu->mem->hist;
  hist_step_t *step = calloc(1, sizeof *step);

  /* fixed size CPU state pre-image; pointer targets keep living in the emu */
  step->regs = emu->x86;
  step->mem_invalid = emu->mem->invalid;

  hist->cur = step;
  hist->recording = 1;
  hist->barrier = 0;
  hist->memfault = 0;
}


/*
 * Record the pre-image of a byte location. Only the first touch within a step
 * is kept, so alias pages and repeated REP writes never overwrite the original
 * content. Linear scan: a single instruction touches only a handful of distinct
 * locations apart from string ops, which are still small in debug sessions.
 */
void emu_hist_record_byte(x86emu_mem_t *mem, u32 addr, u8 data, u8 attr)
{
  x86emu_hist_t *hist = mem->hist;
  hist_step_t *step;
  unsigned u;

  if(!hist || !hist->recording || !(step = hist->cur)) return;

  for(u = 0; u < step->nbytes; u++) {
    if(step->bytes[u].addr == addr) return;
  }

  if(step->nbytes == step->bytes_cap) {
    step->bytes_cap = step->bytes_cap ? step->bytes_cap * 2 : 16;
    step->bytes = realloc(step->bytes, step->bytes_cap * sizeof *step->bytes);
  }

  step->bytes[step->nbytes].addr = addr;
  step->bytes[step->nbytes].data = data;
  step->bytes[step->nbytes].attr = attr;
  step->nbytes++;
}


void emu_hist_record_new_page(x86emu_mem_t *mem, u32 page_addr)
{
  x86emu_hist_t *hist = mem->hist;
  hist_step_t *step;

  if(!hist || !hist->recording || !(step = hist->cur)) return;

  if(step->npages == step->pages_cap) {
    step->pages_cap = step->pages_cap ? step->pages_cap * 2 : 4;
    step->pages = realloc(step->pages, step->pages_cap * sizeof *step->pages);
  }

  step->pages[step->npages++].page_addr = page_addr;
}


/* undo every change of a step, in reverse acquisition order */
static void rollback_step(x86emu_t *emu, hist_step_t *step)
{
  int u;

  /* bytes first while every page involved is still materialized */
  for(u = (int) step->nbytes - 1; u >= 0; u--) {
    emu_hist_restore_byte(emu->mem, step->bytes[u].addr,
      step->bytes[u].data, step->bytes[u].attr);
  }

  /* then remove pages that did not exist before the step */
  for(u = (int) step->npages - 1; u >= 0; u--) {
    emu_hist_dematerialize_page(emu->mem, step->pages[u].page_addr);
  }

  emu->x86 = step->regs;
  emu->mem->invalid = step->mem_invalid;
}


void emu_hist_abort(x86emu_t *emu)
{
  x86emu_hist_t *hist = emu->mem->hist;
  hist_step_t *step = hist->cur;

  hist->recording = 0;
  hist->cur = NULL;

  if(step) {
    rollback_step(emu, step);
    free_step(step);
  }
}


void emu_hist_commit(x86emu_t *emu, int halted)
{
  x86emu_hist_t *hist = emu->mem->hist;
  hist_step_t *step = hist->cur;

  hist->recording = 0;
  hist->cur = NULL;

  if(!step) return;

  step->halted = halted;
  step->next = NULL;
  step->prev = hist->tail;

  if(hist->tail) hist->tail->next = step;
  else hist->head = step;
  hist->tail = step;

  hist->entries++;
  hist->data_bytes += step->nbytes;
  hist->attr_bytes += step->nbytes;
  hist->page_events += step->npages;
}


int emu_hist_undo(x86emu_t *emu)
{
  x86emu_hist_t *hist = emu->mem->hist;
  hist_step_t *step;

  if(!hist) return X86EMU_HIST_BACK_ERROR;
  if(!hist->tail) return X86EMU_HIST_BACK_EMPTY;

  step = hist->tail;
  rollback_step(emu, step);

  hist->tail = step->prev;
  if(hist->tail) hist->tail->next = NULL;
  else hist->head = NULL;

  hist->entries--;
  hist->data_bytes -= step->nbytes;
  hist->attr_bytes -= step->nbytes;
  hist->page_events -= step->npages;

  free_step(step);

  return X86EMU_HIST_BACK_OK;
}


/*------------------------------ public API -------------------------------*/


API_SYM int x86emu_history_enable(x86emu_t *emu)
{
  x86emu_hist_t *hist;

  if(!emu || !emu->mem) return 0;

  /*
   * A custom memio or code handler may touch host state in ways the change
   * recorder cannot see or undo; refuse rather than promise reversibility we
   * cannot keep.
   */
  if(emu->memio != vm_memio || emu->code_check != NULL) return 0;

  if(emu->mem->hist) {
    /* re-enabling on an instruction boundary starts an empty history */
    emu_hist_invalidate(emu);
    return 1;
  }

  hist = emu_hist_new();
  if(!hist) return 0;

  emu->mem->hist = hist;

  return 1;
}


API_SYM void x86emu_history_disable(x86emu_t *emu)
{
  if(!emu || !emu->mem || !emu->mem->hist) return;

  emu->mem->hist = emu_hist_free(emu->mem->hist);
}


API_SYM int x86emu_history_step(x86emu_t *emu)
{
  x86emu_hist_t *hist;
  int exec_rc;

  if(!emu_hist_active(emu)) return X86EMU_HIST_ERROR;

  hist = emu->mem->hist;
  if(hist->recording) return X86EMU_HIST_ERROR;

  emu_hist_begin(emu);

  exec_rc = emu_hist_exec_one(emu);

  if(exec_rc == 2 || hist->barrier) {
    /* host-visible instruction: nothing ran, drop the tentative step */
    emu_hist_abort(emu);
    return X86EMU_HIST_BARRIER;
  }

  if(hist->memfault) {
    /* permission failure: publish nothing, restore the whole instruction */
    emu_hist_abort(emu);
    return X86EMU_HIST_MEMFAULT;
  }

  emu_hist_commit(emu, exec_rc == 1);

  return exec_rc == 1 ? X86EMU_HIST_HALTED : X86EMU_HIST_OK;
}


API_SYM int x86emu_history_back(x86emu_t *emu)
{
  if(!emu || !emu->mem) return X86EMU_HIST_BACK_ERROR;

  return emu_hist_undo(emu);
}


API_SYM int x86emu_history_get_stats(x86emu_t *emu, x86emu_history_stats_t *stats)
{
  x86emu_hist_t *hist;

  if(!emu_hist_active(emu) || !stats) return -1;

  hist = emu->mem->hist;

  stats->entries = hist->entries;
  stats->data_bytes = hist->data_bytes;
  stats->attr_bytes = hist->attr_bytes;
  stats->page_events = hist->page_events;
  stats->cpu_bytes = (unsigned long) hist->entries * sizeof(x86emu_regs_t);

  return 0;
}


API_SYM int x86emu_history_len(x86emu_t *emu)
{
  if(!emu_hist_active(emu)) return 0;

  return (int) emu->mem->hist->entries;
}


API_SYM unsigned x86emu_history_peek(x86emu_t *emu, unsigned addr, unsigned len,
  unsigned char *data, unsigned char *attr)
{
  if(!emu || !emu->mem) return 0;

  return emu_hist_peek(emu->mem, addr, len, data, attr);
}
