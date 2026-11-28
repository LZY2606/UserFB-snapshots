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
*   Memory and i/o access emulation functions.
*
****************************************************************************/


#include "include/x86emu_int.h"
#include "include/history_int.h"
#if defined(__i386__) || defined (__x86_64__)
#include <sys/io.h>
#else
/* define some dummy macros; see also WITH_IOPL in x86emu_int.h */
#define inb(addr) 0xff
#define inw(addr) 0xffff
#define inl(addr) 0xffffffff
#define outb(val, addr)
#define outw(val, addr)
#define outl(val, addr)
#endif

#define PERM16(a)	((a) + ((a) << 8))
#define PERM32(a)	(PERM16(a) + (PERM16(a) << 16))

// avoid unaligned memory accesses
#define STRICT_ALIGN	0

static unsigned vm_r_byte(x86emu_mem_t *vm, unsigned addr);
static unsigned vm_r_byte_noperm(x86emu_mem_t *vm, unsigned addr);
static unsigned vm_r_word(x86emu_mem_t *vm, unsigned addr);
static unsigned vm_r_dword(x86emu_mem_t *vm, unsigned addr);
static unsigned vm_x_byte(x86emu_mem_t *vm, unsigned addr);
static unsigned vm_x_word(x86emu_mem_t *vm, unsigned addr);
static unsigned vm_x_dword(x86emu_mem_t *vm, unsigned addr);
static void vm_w_byte(x86emu_mem_t *vm, unsigned addr, unsigned val);
static void vm_w_byte_noperm(x86emu_mem_t *vm, unsigned addr, unsigned val);
static void vm_w_word(x86emu_mem_t *vm, unsigned addr, unsigned val);
static void vm_w_dword(x86emu_mem_t *vm, unsigned addr, unsigned val);

static mem2_page_t *vm_get_page(x86emu_mem_t *mem, unsigned addr, int create);
static unsigned vm_i_byte(x86emu_t *emu, unsigned addr);
static unsigned vm_i_dword(x86emu_t *emu, unsigned addr);
static unsigned vm_i_word(x86emu_t *emu, unsigned addr);
static void vm_o_byte(x86emu_t *emu, unsigned addr, unsigned val);
static void vm_o_dword(x86emu_t *emu, unsigned addr, unsigned val);
static void vm_o_word(x86emu_t *emu, unsigned addr, unsigned val);

void *mem_dup(const void *src, size_t n)
{
  void *dst;

  if(!src || !n || !(dst = malloc(n))) return NULL;

  memcpy(dst, src, n);

  return dst;
}


x86emu_mem_t *emu_mem_new(unsigned perm)
{
  x86emu_mem_t *mem;

  mem = calloc(1, sizeof *mem);
  mem->def_attr = perm;

  return mem;
}


x86emu_mem_t *emu_mem_free(x86emu_mem_t *mem)
{
  mem2_pdir_t *pdir;
  mem2_ptable_t *ptable;
  mem2_page_t page;
  unsigned pdir_idx, u1;

  if(mem) {
    mem->hist = emu_hist_free(mem->hist);

    if((pdir = mem->pdir)) {
      for(pdir_idx = 0; pdir_idx < (1 << X86EMU_PDIR_BITS); pdir_idx++) {
        ptable = (*pdir)[pdir_idx];
        if(!ptable) continue;
        for(u1 = 0; u1 < (1 << X86EMU_PTABLE_BITS); u1++) {
          page = (*ptable)[u1];
          free(page.attr);
        }
        free(ptable);
      }
      free(pdir);
    }

    free(mem);
  }

  return NULL;
}


x86emu_mem_t *emu_mem_clone(x86emu_mem_t *mem)
{
  mem2_pdir_t *pdir, *new_pdir;
  mem2_ptable_t *ptable, *new_ptable;
  mem2_page_t page;
  unsigned pdir_idx, u1;
  x86emu_mem_t *new_mem = NULL;

  if(!mem) return new_mem;

  new_mem = mem_dup(mem, sizeof *new_mem);

  /* a clone starts outside reversible debugging; never share a history */
  new_mem->hist = NULL;

  if((pdir = mem->pdir)) {
    new_pdir = new_mem->pdir = mem_dup(mem->pdir, sizeof *mem->pdir);
    for(pdir_idx = 0; pdir_idx < (1 << X86EMU_PDIR_BITS); pdir_idx++) {
      ptable = (*pdir)[pdir_idx];
      if(!ptable) continue;
      new_ptable = (*new_pdir)[pdir_idx] = mem_dup(ptable, sizeof *ptable);
      for(u1 = 0; u1 < (1 << X86EMU_PTABLE_BITS); u1++) {
        page = (*ptable)[u1];
        if(page.attr) {
          (*new_ptable)[u1].attr = mem_dup(page.attr, 2 * X86EMU_PAGE_SIZE);
          if(page.data == page.attr + X86EMU_PAGE_SIZE) {
            (*new_ptable)[u1].data = (*new_ptable)[u1].attr + X86EMU_PAGE_SIZE;
          }
        }
      }
    }
  }

  return new_mem;
}


API_SYM void x86emu_reset_access_stats(x86emu_t *emu)
{
  mem2_pdir_t *pdir;
  mem2_ptable_t *ptable;
  mem2_page_t page;
  unsigned pdir_idx, u, u1;

  /* access bits are part of the replayable state; discard old records */
  emu_hist_invalidate(emu);

  if(!emu || !emu->mem || !(pdir = emu->mem->pdir)) return;

  for(pdir_idx = 0; pdir_idx < (1 << X86EMU_PDIR_BITS); pdir_idx++) {
    ptable = (*pdir)[pdir_idx];
    if(!ptable) continue;
    for(u = 0; u < (1 << X86EMU_PTABLE_BITS); u++) {
      page = (*ptable)[u];
      if(page.attr) {
        for(u1 = 0; u1 < X86EMU_PAGE_SIZE; u1++) {
          page.attr[u1] &= X86EMU_PERM_RWX | X86EMU_PERM_VALID;
        }
      }
    }
  }
}


API_SYM void x86emu_set_io_perm(x86emu_t *emu, unsigned start, unsigned end, unsigned perm)
{
  if(!emu) return;

  emu_hist_invalidate(emu);

  if(end > X86EMU_IO_PORTS - 1) end = X86EMU_IO_PORTS - 1;

  while(start <= end) emu->io.map[start++] = perm;

  for(start = perm = 0; start < X86EMU_IO_PORTS; start++) {
    perm |= emu->io.map[start];
  }

  emu->io.iopl_needed = (perm & (X86EMU_PERM_R | X86EMU_PERM_W)) ? 1 : 0;

#if WITH_IOPL 
  emu->io.iopl_ok = emu->io.iopl_needed && getiopl() != 3 ? 0 : 1;
#else 
  emu->io.iopl_ok = 1;
#endif
}


mem2_page_t *vm_get_page(x86emu_mem_t *mem, unsigned addr, int create)
{
  mem2_pdir_t *pdir;
  mem2_ptable_t *ptable;
  mem2_page_t page;
  unsigned pdir_idx = addr >> (32 - X86EMU_PDIR_BITS);
  unsigned ptable_idx = (addr >> X86EMU_PAGE_BITS) & ((1 << X86EMU_PTABLE_BITS) - 1);
  unsigned u;

  pdir = mem->pdir;
  if(!pdir) {
    mem->pdir = pdir = calloc(1, sizeof *pdir);
    // fprintf(stderr, "pdir = %p (%d)\n", pdir, sizeof *pdir);
  }

  ptable = (*pdir)[pdir_idx];
  if(!ptable) {
    ptable = (*pdir)[pdir_idx] = calloc(1, sizeof *ptable);
    // fprintf(stderr, "ptable = %p\n", ptable);
    for(u = 0; u < (1 << X86EMU_PTABLE_BITS); u++) {
      (*ptable)[u].def_attr = mem->def_attr;
      // fprintf(stderr, "ptable[%u] = %p\n", u, &((*ptable)[u].def_attr));
    }
    // fprintf(stderr, "pdir[%d] = %p (%d)\n", pdir_idx, ptable, sizeof *ptable);
  }

  if(create) {
    page = (*ptable)[ptable_idx];
    if(!page.attr) {
      page.attr = calloc(1, 2 * X86EMU_PAGE_SIZE);
      page.data = page.attr + X86EMU_PAGE_SIZE;
      // fprintf(stderr, "page = %p, page.def_attr = %p\n", page, &page.def_attr);
      memset(page.attr, page.def_attr, X86EMU_PAGE_SIZE);
      (*ptable)[ptable_idx] = page;
      /* remember that this sparse page did not exist before the step */
      emu_hist_record_new_page(mem,
        (pdir_idx << (32 - X86EMU_PDIR_BITS)) | (ptable_idx << X86EMU_PAGE_BITS));
    }
  }

  return (*ptable) + ptable_idx;
}


API_SYM void x86emu_set_perm(x86emu_t *emu, unsigned start, unsigned end, unsigned perm)
{
  x86emu_mem_t *mem;
  mem2_page_t *page;
  unsigned idx;

  /* permission changes cannot be represented by the existing change records */
  emu_hist_invalidate(emu);

  if(!emu || !(mem = emu->mem)) return;

  if(start > end) return;

  // x86emu_log(emu, "set perm: start 0x%x, end 0x%x, perm 0x%x\n", start, end, perm);

  if((idx = start & (X86EMU_PAGE_SIZE - 1))) {
    page = vm_get_page(mem, start, 1);
    for(; idx < X86EMU_PAGE_SIZE && start <= end; start++) {
      // x86emu_log(emu, "  page %p, idx = 0x%x\n", page, idx);
      page->attr[idx++] = perm;
    }
    if(!start || start > end) return;
  }

  // x86emu_log(emu, "  2: start 0x%x, end 0x%x\n", start, end);

  for(; end - start >= X86EMU_PAGE_SIZE - 1; start += X86EMU_PAGE_SIZE) {
    page = vm_get_page(mem, start, 0);
    page->def_attr = perm;
    // x86emu_log(emu, "  page %p (start 0x%x, end - start 0x%x)\n", page, start, end - start);
    if(page->attr) memset(page->attr, page->def_attr, X86EMU_PAGE_SIZE);
    if(!start) return;
    if(end - start == X86EMU_PAGE_SIZE - 1) {
      start += X86EMU_PAGE_SIZE;
      break;
    }
  }

  if(start > end) return;

  // x86emu_log(emu, "  3: start 0x%x, end 0x%x\n", start, end);

  page = vm_get_page(mem, start, 1);
  end = end - start + 1;
  for(idx = 0; idx < end; idx++) {
    // x86emu_log(emu, "  page %p, idx = 0x%x\n", page, idx);
    page->attr[idx] = perm;
  }
}


API_SYM void x86emu_set_page(x86emu_t *emu, unsigned page, void *address)
{
  x86emu_mem_t *mem;
  mem2_page_t *p;
  unsigned u;

  /* external mappings cannot be reconstructed; drop the old history */
  emu_hist_invalidate(emu);

  if(!emu || !(mem = emu->mem)) return;

  p = vm_get_page(mem, page, 1);

  if(address) {
    p->data = address;

    // tag memory as initialized
    for(u = 0; u < X86EMU_PAGE_SIZE; u++) {
      p->attr[u] |= X86EMU_PERM_VALID;
    }
  }
  else {
    p->data = p->attr + X86EMU_PAGE_SIZE;
  }
}


unsigned vm_r_byte(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *perm;

  page = vm_get_page(mem, addr, 1);
  perm = page->attr + page_idx;

  if(*perm & X86EMU_PERM_R) {
    emu_hist_record_byte(mem, addr, page->data[page_idx], *perm);
    *perm |= X86EMU_ACC_R;
    if(!(*perm & X86EMU_PERM_VALID)) {
      *perm |= X86EMU_ACC_INVALID;
      mem->invalid = 1;
    }
    return page->data[page_idx];
  }

  mem->invalid = 1;
  if(mem->hist) mem->hist->memfault = 1;

  return 0xff;
}


unsigned vm_r_byte_noperm(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  // unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  // attr = page->attr + page_idx;

  return page->data[page_idx];
}


unsigned vm_r_word(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned val, page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  u16 *perm16;

  page = vm_get_page(mem, addr, 1);
  perm16 = (u16 *) (page->attr + page_idx);

  if(
#if STRICT_ALIGN
    (page_idx & 1) ||
#else
    page_idx >= X86EMU_PAGE_SIZE - 1 ||
#endif
    (*perm16 & PERM16(X86EMU_PERM_R | X86EMU_PERM_VALID)) != PERM16(X86EMU_PERM_R | X86EMU_PERM_VALID)
  ) {
    val = vm_r_byte(mem, addr);
    val += vm_r_byte(mem, addr + 1) << 8;

    return val;
  }

  emu_hist_record_byte(mem, addr, page->data[page_idx], page->attr[page_idx]);
  emu_hist_record_byte(mem, addr + 1, page->data[page_idx + 1],
    page->attr[page_idx + 1]);

  *perm16 |= PERM16(X86EMU_ACC_R);

#if defined(__BIG_ENDIAN__) || STRICT_ALIGN
  val = page->data[page_idx] + (page->data[page_idx + 1] << 8);
#else
  val = *(u16 *) (page->data + page_idx);
#endif

  return val;
}


unsigned vm_r_dword(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned val, page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  u32 *perm32;

  page = vm_get_page(mem, addr, 1);
  perm32 = (u32 *) (page->attr + page_idx);

  if(
#if STRICT_ALIGN
    (page_idx & 3) ||
#else
    page_idx >= X86EMU_PAGE_SIZE - 3 ||
#endif
    (*perm32 & PERM32(X86EMU_PERM_R | X86EMU_PERM_VALID)) != PERM32(X86EMU_PERM_R | X86EMU_PERM_VALID)
  ) {
    val = vm_r_byte(mem, addr);
    val += vm_r_byte(mem, addr + 1) << 8;
    val += vm_r_byte(mem, addr + 2) << 16;
    val += vm_r_byte(mem, addr + 3) << 24;

    return val;
  }

  emu_hist_record_byte(mem, addr, page->data[page_idx], page->attr[page_idx]);
  emu_hist_record_byte(mem, addr + 1, page->data[page_idx + 1],
    page->attr[page_idx + 1]);
  emu_hist_record_byte(mem, addr + 2, page->data[page_idx + 2],
    page->attr[page_idx + 2]);
  emu_hist_record_byte(mem, addr + 3, page->data[page_idx + 3],
    page->attr[page_idx + 3]);

  *perm32 |= PERM32(X86EMU_ACC_R);

#if defined(__BIG_ENDIAN__) || STRICT_ALIGN
  val = page->data[page_idx] +
    (page->data[page_idx + 1] << 8) +
    (page->data[page_idx + 2] << 16) +
    (page->data[page_idx + 3] << 24);
#else
  val = *(u32 *) (page->data + page_idx);
#endif

  return val;
}


unsigned vm_x_byte(x86emu_mem_t *mem, unsigned addr)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  attr = page->attr + page_idx;

  if(*attr & X86EMU_PERM_X) {
    emu_hist_record_byte(mem, addr, page->data[page_idx], *attr);
    *attr |= X86EMU_ACC_X;
    if(!(*attr & X86EMU_PERM_VALID)) {
      *attr |= X86EMU_ACC_INVALID;
      mem->invalid = 1;
    }
    return page->data[page_idx];
  }

  mem->invalid = 1;
  if(mem->hist) mem->hist->memfault = 1;

  return 0xff;
}


unsigned vm_x_word(x86emu_mem_t *mem, unsigned addr)
{
  return vm_x_byte(mem, addr) + (vm_x_byte(mem, addr + 1) << 8);
}


unsigned vm_x_dword(x86emu_mem_t *mem, unsigned addr)
{
  return vm_x_word(mem, addr) + (vm_x_word(mem, addr + 2) << 16);
}


void vm_w_byte(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  attr = page->attr + page_idx;

  /* both the success and the invalid path change the attribute byte */
  emu_hist_record_byte(mem, addr, page->data[page_idx], *attr);

  if(*attr & X86EMU_PERM_W) {
    *attr |= X86EMU_PERM_VALID | X86EMU_ACC_W;
    page->data[page_idx] = val;
  }
  else {
    *attr |= X86EMU_ACC_INVALID;

    mem->invalid = 1;
    if(mem->hist) mem->hist->memfault = 1;
  }
}


void vm_w_byte_noperm(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  unsigned char *attr;

  page = vm_get_page(mem, addr, 1);
  attr = page->attr + page_idx;

  emu_hist_record_byte(mem, addr, page->data[page_idx], *attr);

  *attr |= X86EMU_PERM_VALID | X86EMU_ACC_W;
  page->data[page_idx] = val;
}


void vm_w_word(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  u16 *perm16;

  page = vm_get_page(mem, addr, 1);
  perm16 = (u16 *) (page->attr + page_idx);

  if(
#if STRICT_ALIGN
    (page_idx & 1) ||
#else
    page_idx >= X86EMU_PAGE_SIZE - 1 ||
#endif
    (*perm16 & PERM16(X86EMU_PERM_W)) != PERM16(X86EMU_PERM_W)
  ) {
    vm_w_byte(mem, addr, val);
    vm_w_byte(mem, addr + 1, val >> 8);

    return;
  }

  emu_hist_record_byte(mem, addr, page->data[page_idx], page->attr[page_idx]);
  emu_hist_record_byte(mem, addr + 1, page->data[page_idx + 1],
    page->attr[page_idx + 1]);

  *perm16 |= PERM16(X86EMU_PERM_VALID | X86EMU_ACC_W);

#if defined(__BIG_ENDIAN__) || STRICT_ALIGN
  page->data[page_idx] = val;
  page->data[page_idx + 1] = val >> 8;
#else
  *(u16 *) (page->data + page_idx) = val;
#endif
}


void vm_w_dword(x86emu_mem_t *mem, unsigned addr, unsigned val)
{
  mem2_page_t *page;
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);
  u32 *perm32;

  page = vm_get_page(mem, addr, 1);
  perm32 = (u32 *) (page->attr + page_idx);

  if(
#if STRICT_ALIGN
    (page_idx & 3) ||
#else
    page_idx >= X86EMU_PAGE_SIZE - 3 ||
#endif
    (*perm32 & PERM32(X86EMU_PERM_W)) != PERM32(X86EMU_PERM_W)
  ) {
    vm_w_byte(mem, addr, val);
    vm_w_byte(mem, addr + 1, val >> 8);
    vm_w_byte(mem, addr + 2, val >> 16);
    vm_w_byte(mem, addr + 3, val >> 24);

    return;
  }

  emu_hist_record_byte(mem, addr, page->data[page_idx], page->attr[page_idx]);
  emu_hist_record_byte(mem, addr + 1, page->data[page_idx + 1],
    page->attr[page_idx + 1]);
  emu_hist_record_byte(mem, addr + 2, page->data[page_idx + 2],
    page->attr[page_idx + 2]);
  emu_hist_record_byte(mem, addr + 3, page->data[page_idx + 3],
    page->attr[page_idx + 3]);

  *perm32 |= PERM32(X86EMU_PERM_VALID | X86EMU_ACC_W);

#if defined(__BIG_ENDIAN__) || STRICT_ALIGN
  page->data[page_idx] = val;
  page->data[page_idx + 1] = val >> 8;
  page->data[page_idx + 2] = val >> 16;
  page->data[page_idx + 3] = val >> 24;
#else
  *(u32 *) (page->data + page_idx) = val;
#endif
}


unsigned vm_i_byte(x86emu_t *emu, unsigned addr)
{
  unsigned char *perm;

  addr &= 0xffff;
  perm = emu->io.map + addr;

  if(
    emu->io.iopl_ok &&
    (*perm & X86EMU_PERM_R)
  ) {
    *perm |= X86EMU_ACC_R;

    emu->io.stats_i[addr]++;

    return inb(addr);
  }
  else {
    *perm |= X86EMU_ACC_INVALID;
  }

  emu->mem->invalid = 1;

  return 0xff;
}


unsigned vm_i_word(x86emu_t *emu, unsigned addr)
{
  unsigned char *perm;
  unsigned val;

  addr &= 0xffff;
  perm = emu->io.map + addr;

  if(
    !emu->io.iopl_ok ||
    addr == 0xffff ||
    !(perm[0] & X86EMU_PERM_R) ||
    !(perm[1] & X86EMU_PERM_R)
  ) {
    val = vm_i_byte(emu, addr);
    val += (vm_i_byte(emu, addr + 1) << 8);

    return val;
  }

  perm[0] |= X86EMU_ACC_R;
  perm[1] |= X86EMU_ACC_R;

  emu->io.stats_i[addr]++;
  emu->io.stats_i[addr + 1]++;

  return inw(addr);
}


unsigned vm_i_dword(x86emu_t *emu, unsigned addr)
{
  unsigned char *perm;
  unsigned val;

  addr &= 0xffff;
  perm = emu->io.map + addr;

  if(
    !emu->io.iopl_ok ||
    addr >= 0xfffd ||
    !(perm[0] & X86EMU_PERM_R) ||
    !(perm[1] & X86EMU_PERM_R) ||
    !(perm[2] & X86EMU_PERM_R) ||
    !(perm[3] & X86EMU_PERM_R)
  ) {
    val = vm_i_byte(emu, addr);
    val += (vm_i_byte(emu, addr + 1) << 8);
    val += (vm_i_byte(emu, addr + 2) << 16);
    val += (vm_i_byte(emu, addr + 3) << 24);

    return val;
  }

  perm[0] |= X86EMU_ACC_R;
  perm[1] |= X86EMU_ACC_R;
  perm[2] |= X86EMU_ACC_R;
  perm[3] |= X86EMU_ACC_R;

  emu->io.stats_i[addr]++;
  emu->io.stats_i[addr + 1]++;
  emu->io.stats_i[addr + 2]++;
  emu->io.stats_i[addr + 3]++;

  return inl(addr);
}


void vm_o_byte(x86emu_t *emu, unsigned addr, unsigned val)
{
  unsigned char *perm;

  addr &= 0xffff;
  perm = emu->io.map + addr;

  if(
    emu->io.iopl_ok &&
    (*perm & X86EMU_PERM_W)
  ) {
    *perm |= X86EMU_ACC_W;

    emu->io.stats_o[addr]++;

    outb(val, addr);
  }
  else {
    *perm |= X86EMU_ACC_INVALID;

    emu->mem->invalid = 1;
  }
}


void vm_o_word(x86emu_t *emu, unsigned addr, unsigned val)
{
  unsigned char *perm;

  addr &= 0xffff;
  perm = emu->io.map + addr;

  if(
    !emu->io.iopl_ok ||
    addr == 0xffff ||
    !(perm[0] & X86EMU_PERM_W) ||
    !(perm[1] & X86EMU_PERM_W)
  ) {
    vm_o_byte(emu, addr, val);
    vm_o_byte(emu, addr + 1, val);

    return;
  }

  perm[0] |= X86EMU_ACC_W;
  perm[1] |= X86EMU_ACC_W;

  emu->io.stats_o[addr]++;
  emu->io.stats_o[addr + 1]++;

  outw(val, addr);
}


void vm_o_dword(x86emu_t *emu, unsigned addr, unsigned val)
{
  unsigned char *perm;

  addr &= 0xffff;
  perm = emu->io.map + addr;

  if(
    !emu->io.iopl_ok ||
    addr >= 0xfffd ||
    !(perm[0] & X86EMU_PERM_W) ||
    !(perm[1] & X86EMU_PERM_W) ||
    !(perm[2] & X86EMU_PERM_W) ||
    !(perm[3] & X86EMU_PERM_W)
  ) {
    vm_o_byte(emu, addr, val);
    vm_o_byte(emu, addr + 1, val);
    vm_o_byte(emu, addr + 2, val);
    vm_o_byte(emu, addr + 3, val);

    return;
  }

  perm[0] |= X86EMU_ACC_W;
  perm[1] |= X86EMU_ACC_W;
  perm[2] |= X86EMU_ACC_W;
  perm[3] |= X86EMU_ACC_W;

  emu->io.stats_o[addr]++;
  emu->io.stats_o[addr + 1]++;
  emu->io.stats_o[addr + 2]++;
  emu->io.stats_o[addr + 3]++;

  outl(val, addr);
}


unsigned vm_memio(x86emu_t *emu, u32 addr, u32 *val, unsigned type)
{
  x86emu_mem_t *mem = emu->mem;
  unsigned bits = type & 0xff;

  type &= ~0xff;

  /*
   * In reversible mode port I/O may touch the host. Refuse before any handler
   * runs and leave the machine state untouched; the step is reported as a
   * barrier and not recorded.
   */
  if(mem->hist && mem->hist->recording &&
    (type == X86EMU_MEMIO_I || type == X86EMU_MEMIO_O)) {
    mem->hist->barrier = 1;
    if(val) *val = bits == X86EMU_MEMIO_8 ? 0xff :
      bits == X86EMU_MEMIO_16 ? 0xffff : 0xffffffffu;
    return 0;
  }

  mem->invalid = 0;

  switch(type) {
    case X86EMU_MEMIO_R:
      switch(bits) {
        case X86EMU_MEMIO_8:
          *val = vm_r_byte(mem, addr);
          break;
        case X86EMU_MEMIO_16:
          *val = vm_r_word(mem, addr);
          break;
        case X86EMU_MEMIO_32:
          *val = vm_r_dword(mem, addr);
          break;
        case X86EMU_MEMIO_8_NOPERM:
          *val = vm_r_byte_noperm(mem, addr);
          break;
      }
      break;

    case X86EMU_MEMIO_W:
      switch(bits) {
        case X86EMU_MEMIO_8:
          vm_w_byte(mem, addr, *val);
          break;
        case X86EMU_MEMIO_16:
          vm_w_word(mem, addr, *val);
          break;
        case X86EMU_MEMIO_32:
          vm_w_dword(mem, addr, *val);
          break;
        case X86EMU_MEMIO_8_NOPERM:
          vm_w_byte_noperm(mem, addr, *val);
          break;
      }
      break;

    case X86EMU_MEMIO_X:
      switch(bits) {
        case X86EMU_MEMIO_8:
          *val = vm_x_byte(mem, addr);
          break;
        case X86EMU_MEMIO_16:
          *val = vm_x_word(mem, addr);
          break;
        case X86EMU_MEMIO_32:
          *val = vm_x_dword(mem, addr);
          break;
      }
      break;

    case X86EMU_MEMIO_I:
      switch(bits) {
        case X86EMU_MEMIO_8:
          *val = vm_i_byte(emu, addr);
          break;
        case X86EMU_MEMIO_16:
          *val = vm_i_word(emu, addr);
          break;
        case X86EMU_MEMIO_32:
          *val = vm_i_dword(emu, addr);
          break;
      }
      break;

    case X86EMU_MEMIO_O:
      switch(bits) {
        case X86EMU_MEMIO_8:
          vm_o_byte(emu, addr, *val);
          break;
        case X86EMU_MEMIO_16:
          vm_o_word(emu, addr, *val);
          break;
        case X86EMU_MEMIO_32:
          vm_o_dword(emu, addr, *val);
          break;
      }
      break;
  }

  return mem->invalid;
}


/*
 * Restore one byte location during an undo. This deliberately bypasses the
 * normal accessors so rolling back never creates additional write/access
 * records. The page is guaranteed to still be materialized because new pages
 * are only dematerialized after all byte changes have been undone.
 */
void emu_hist_restore_byte(x86emu_mem_t *mem, u32 addr, u8 data, u8 attr)
{
  mem2_page_t *page = vm_get_page(mem, addr, 0);
  unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);

  if(!page || !page->attr) return;

  page->attr[page_idx] = attr;
  page->data[page_idx] = data;
}


/*
 * Return a page that was created inside a step to its previous, unmaterialized
 * state. The backing memory is library allocated (only such pages are
 * recorded here); external x86emu_set_page mappings are restored in place via
 * byte records and never dematerialized.
 */
void emu_hist_dematerialize_page(x86emu_mem_t *mem, u32 page_addr)
{
  mem2_pdir_t *pdir;
  mem2_ptable_t *ptable;
  mem2_page_t *page;
  unsigned pdir_idx = page_addr >> (32 - X86EMU_PDIR_BITS);
  unsigned ptable_idx = (page_addr >> X86EMU_PAGE_BITS) &
    ((1 << X86EMU_PTABLE_BITS) - 1);
  unsigned char def_attr;

  if(!(pdir = mem->pdir) || !(ptable = (*pdir)[pdir_idx])) return;

  page = (*ptable) + ptable_idx;
  if(!page->attr) return;

  def_attr = page->def_attr;
  free(page->attr);
  *page = (mem2_page_t) { .def_attr = def_attr };
}


/*
 * Non disturbing observer: copy data and attributes without changing access
 * bits or materializing a sparse page. Unmaterialized bytes read as 0xff with
 * the memory default attribute.
 */
unsigned emu_hist_peek(x86emu_mem_t *mem, unsigned addr, unsigned len,
  unsigned char *data, unsigned char *attr)
{
  unsigned u;

  if(!mem) return 0;

  for(u = 0; u < len; u++, addr++) {
    mem2_pdir_t *pdir = mem->pdir;
    mem2_ptable_t *ptable;
    mem2_page_t *page;
    unsigned pdir_idx = addr >> (32 - X86EMU_PDIR_BITS);
    unsigned ptable_idx = (addr >> X86EMU_PAGE_BITS) &
      ((1 << X86EMU_PTABLE_BITS) - 1);
    unsigned page_idx = addr & (X86EMU_PAGE_SIZE - 1);

    if(!pdir || !(ptable = (*pdir)[pdir_idx]) ||
      !(page = (*ptable) + ptable_idx) || !page->attr) {
      if(data) data[u] = 0xff;
      if(attr) attr[u] = mem->def_attr;
      continue;
    }

    if(data) data[u] = page->data[page_idx];
    if(attr) {
      attr[u] = page->attr ? page->attr[page_idx] : page->def_attr;
    }
  }

  return len;
}

