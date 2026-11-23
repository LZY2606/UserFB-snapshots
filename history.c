/****************************************************************************
*
* Realmode X86 Emulator Library
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
*   Reversible debugging ("history") support.
*
*   Records sparse pre-images (CPU state + actually changed memory data and
*   attribute bytes) per executed instruction so a debugging session can
*   step forwards and backwards without re-running from the start.
*
****************************************************************************/


#include "include/x86emu_int.h"


/* byte record kinds */
#define HREC_ATTR	0	/* memory attribute byte */
#define HREC_DATA	1	/* memory data byte */

/* structural record kinds */
#define SREC_PAGE	0	/* sparse page was materialized */
#define SREC_PTABLE	1	/* page table was created */
#define SREC_PDIR	2	/* page directory was created */


typedef struct {
  u8 *ptr;			/* host address of the byte */
  u8 val;			/* value before the step */
  u8 kind;			/* HREC_* */
} hrec_byte_t;

typedef struct {
  u32 pdi;			/* page directory index */
  u32 pti;			/* page table index (SREC_PAGE only) */
  u8 kind;			/* SREC_* */
  u8 def_attr;			/* page default attr before the step (SREC_PAGE) */
} hrec_struct_t;

typedef struct {
  x86emu_regs_t regs;		/* fixed size cpu pre-image (msr pointers shared) */
  u64 tsc;			/* msr[0x10] before the step */
  unsigned mem_invalid;		/* mem->invalid before the step */
  hrec_byte_t *bytes;		/* sparse data/attr pre-images */
  size_t bytes_n, bytes_cap;
  size_t *hash;			/* ptr -> bytes index + 1 (dedup) */
  size_t hash_cap, hash_n;
  hrec_struct_t *structs;	/* page/ptable/pdir creation records */
  size_t structs_n, structs_cap;
  u32 last_pg, last_pt;		/* dedup helpers for a single access */
  int pdir_rec;
  u64 data_bytes, attr_bytes;	/* statistics */
} hist_entry_t;

typedef struct x86emu_hist_s {
  hist_entry_t *entries;	/* stack of undoable steps */
  size_t count, cap;
  hist_entry_t *cur;		/* entry being recorded */
  int recording;		/* inside x86emu_history_step() */
  int memerr;			/* current step saw a permission failure */
  int barrier;			/* current step hit a side effect barrier */
} hist_t;


static unsigned hist_memio(x86emu_t *emu, u32 addr, u32 *val, unsigned type);
static mem2_page_t *hist_find_page(x86emu_mem_t *mem, u32 addr);
static void hist_rec_page(x86emu_t *emu, u32 addr);
static void hist_rec_byte(x86emu_t *emu, u8 *ptr, int kind);
static void hist_rec_struct(x86emu_t *emu, int kind, u32 pdi, u32 pti, u8 def_attr);
static void hist_rollback(x86emu_t *emu, hist_entry_t *e);
static int hist_barrier_ahead(x86emu_t *emu);
static u8 hist_peek_byte(x86emu_t *emu, u32 addr, u8 *attr);
static hist_entry_t *hist_entry_push(hist_t *h);
static void hist_entry_clear(hist_entry_t *e);


/*
 * Find the page struct for addr without creating anything.
 */
static mem2_page_t *hist_find_page(x86emu_mem_t *mem, u32 addr)
{
  mem2_ptable_t *ptable;

  if(!mem->pdir) return NULL;

  ptable = (*mem->pdir)[addr >> (32 - X86EMU_PDIR_BITS)];
  if(!ptable) return NULL;

  return &(*ptable)[(addr >> X86EMU_PAGE_BITS) & ((1 << X86EMU_PTABLE_BITS) - 1)];
}


/*
 * Read a single byte + attributes without side effects: no access bits are
 * set and no sparse page is materialized. Unallocated bytes read as 0xff
 * with the default attributes.
 */
static u8 hist_peek_byte(x86emu_t *emu, u32 addr, u8 *attr)
{
  mem2_page_t *pg = hist_find_page(emu->mem, addr);
  unsigned ofs = addr & (X86EMU_PAGE_SIZE - 1);
  u8 a, d;

  if(pg && pg->attr) {
    a = pg->attr[ofs];
    d = pg->data[ofs];
  }
  else {
    a = pg ? pg->def_attr : emu->mem->def_attr;
    d = 0xff;
  }

  if(attr) *attr = a;

  return d;
}


API_SYM void x86emu_history_peek(x86emu_t *emu, u32 addr, unsigned len, u8 *data, u8 *attr)
{
  unsigned u;

  if(!emu || !emu->mem) return;

  for(u = 0; u < len; u++) {
    u8 a, d;

    d = hist_peek_byte(emu, addr + u, &a);

    if(data) data[u] = d;
    if(attr) attr[u] = a;
  }
}


static size_t hist_ptr_hash(u8 *ptr)
{
  uintptr_t x = (uintptr_t) ptr;

  x >>= 4;
  x *= (uintptr_t) 0x9e3779b1;
  x ^= x >> 16;

  return (size_t) x;
}


/*
 * Record the current value of a host memory byte (memory data or attribute
 * byte). Only the first touch within a step is recorded, so aliased guest
 * pages (several guest pages mapping the same host storage via
 * x86emu_set_page) and repeated writes to the same storage (e.g. REP
 * string ops) keep the true pre-instruction value.
 */
static void hist_rec_byte(x86emu_t *emu, u8 *ptr, int kind)
{
  hist_t *h = emu->hist;
  hist_entry_t *e = h->cur;
  size_t mask, i;

  if(!e->hash) {
    e->hash_cap = 64;
    e->hash = calloc(e->hash_cap, sizeof *e->hash);
  }

  for(;;) {
    mask = e->hash_cap - 1;

    if((e->hash_n + 1) * 4 >= e->hash_cap * 3) {
      size_t new_cap = e->hash_cap * 2, u;
      size_t *new_hash = calloc(new_cap, sizeof *new_hash);

      for(u = 0; u < e->hash_cap; u++) {
        if(e->hash[u]) {
          size_t k = hist_ptr_hash(e->bytes[e->hash[u] - 1].ptr) & (new_cap - 1);
          while(new_hash[k]) k = (k + 1) & (new_cap - 1);
          new_hash[k] = e->hash[u];
        }
      }

      free(e->hash);
      e->hash = new_hash;
      e->hash_cap = new_cap;

      continue;
    }

    i = hist_ptr_hash(ptr) & mask;
    while(e->hash[i]) {
      if(e->bytes[e->hash[i] - 1].ptr == ptr) return;
      i = (i + 1) & mask;
    }

    break;
  }

  if(e->bytes_n == e->bytes_cap) {
    e->bytes_cap = e->bytes_cap ? 2 * e->bytes_cap : 64;
    e->bytes = realloc(e->bytes, e->bytes_cap * sizeof *e->bytes);
  }

  e->bytes[e->bytes_n].ptr = ptr;
  e->bytes[e->bytes_n].val = *ptr;
  e->bytes[e->bytes_n].kind = kind;
  e->hash[i] = ++e->bytes_n;
  e->hash_n++;

  if(kind == HREC_ATTR) {
    e->attr_bytes++;
  }
  else {
    e->data_bytes++;
  }
}


static void hist_rec_struct(x86emu_t *emu, int kind, u32 pdi, u32 pti, u8 def_attr)
{
  hist_entry_t *e = emu->hist->cur;

  if(e->structs_n == e->structs_cap) {
    e->structs_cap = e->structs_cap ? 2 * e->structs_cap : 8;
    e->structs = realloc(e->structs, e->structs_cap * sizeof *e->structs);
  }

  e->structs[e->structs_n].kind = kind;
  e->structs[e->structs_n].pdi = pdi;
  e->structs[e->structs_n].pti = pti;
  e->structs[e->structs_n].def_attr = def_attr;
  e->structs_n++;
}


/*
 * Record that the access at addr is about to materialize a sparse page
 * (and possibly its page table and the page directory). Called just before
 * the underlying memio access that would create them.
 */
static void hist_rec_page(x86emu_t *emu, u32 addr)
{
  hist_entry_t *e = emu->hist->cur;
  x86emu_mem_t *mem = emu->mem;
  u32 pdi = addr >> (32 - X86EMU_PDIR_BITS);
  u32 pti = (addr >> X86EMU_PAGE_BITS) & ((1 << X86EMU_PTABLE_BITS) - 1);
  u32 frame = addr >> X86EMU_PAGE_BITS;
  mem2_ptable_t *ptable = mem->pdir ? (*mem->pdir)[pdi] : NULL;

  if(ptable && (*ptable)[pti].attr) return;	/* already materialized */
  if(frame == e->last_pg) return;		/* already recorded */
  e->last_pg = frame;

  if(!mem->pdir && !e->pdir_rec) {
    hist_rec_struct(emu, SREC_PDIR, 0, 0, 0);
    e->pdir_rec = 1;
  }

  if(!ptable && pdi != e->last_pt) {
    hist_rec_struct(emu, SREC_PTABLE, pdi, 0, 0);
    e->last_pt = pdi;
  }

  hist_rec_struct(emu, SREC_PAGE, pdi, pti, ptable ? (*ptable)[pti].def_attr : mem->def_attr);
}


/*
 * Recording memio wrapper. Installed instead of the default vm_memio while
 * history mode is active. Records sparse pre-images of everything the
 * emulated instruction is about to change and detects memory permission
 * failures and side effect barriers.
 */
static unsigned hist_memio(x86emu_t *emu, u32 addr, u32 *val, unsigned type)
{
  hist_t *h = emu->hist;
  x86emu_mem_t *mem;
  unsigned bits = type & 0xff, t = type & ~0xff;
  unsigned len, u, need;
  int noperm;

  if(!h || !h->recording || !emu->mem) return vm_memio(emu, addr, val, type);

  mem = emu->mem;

  if(t == X86EMU_MEMIO_I || t == X86EMU_MEMIO_O) {
    /* port i/o: never touch the host from a replayable step */
    h->barrier = 1;

    return 0;
  }

  len = bits == X86EMU_MEMIO_16 ? 2 : bits == X86EMU_MEMIO_32 ? 4 : 1;
  noperm = bits == X86EMU_MEMIO_8_NOPERM;

  /* an access spans at most two pages */
  hist_rec_page(emu, addr);
  if((addr >> X86EMU_PAGE_BITS) != ((addr + len - 1) >> X86EMU_PAGE_BITS)) {
    hist_rec_page(emu, addr + len - 1);
  }

  need =
    t == X86EMU_MEMIO_W ? X86EMU_PERM_W :
    t == X86EMU_MEMIO_X ? X86EMU_PERM_X : X86EMU_PERM_R;

  for(u = 0; u < len; u++) {
    u32 a = addr + u;
    mem2_page_t *pg = hist_find_page(mem, a);
    unsigned ofs = a & (X86EMU_PAGE_SIZE - 1);
    unsigned char attr = pg ? (pg->attr ? pg->attr[ofs] : pg->def_attr) : mem->def_attr;

    if(!noperm) {
      if(!(attr & need)) {
        h->memerr = 1;
      }
      else if(t == X86EMU_MEMIO_X && !(attr & X86EMU_PERM_VALID)) {
        /* instruction fetch from invalid memory stops the cpu */
        h->memerr = 1;
      }
    }

    if(pg && pg->attr) {
      /* pages materialized by this very step are covered by SREC_PAGE */
      if(!(noperm && t == X86EMU_MEMIO_R)) hist_rec_byte(emu, pg->attr + ofs, HREC_ATTR);
      if(t == X86EMU_MEMIO_W) hist_rec_byte(emu, pg->data + ofs, HREC_DATA);
    }
  }

  return vm_memio(emu, addr, val, type);
}


/*
 * Classify the next instruction without any side effects. Returns non-zero
 * if executing it could observe or change the host (port i/o, external
 * interrupt/cpuid/msr handlers, timestamp reads).
 */
static int hist_barrier_ahead(x86emu_t *emu)
{
  u32 addr = emu->x86.R_CS_BASE + emu->x86.R_EIP;
  u8 op = 0;
  int u;

  for(u = 0; u < 15; u++) {
    op = hist_peek_byte(emu, addr, NULL);
    switch(op) {
      case 0x26: case 0x2e: case 0x36: case 0x3e:
      case 0x64: case 0x65: case 0x66: case 0x67:
      case 0xf0: case 0xf2: case 0xf3:
        addr++;
        continue;
    }
    break;
  }

  switch(op) {
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:	/* ins/outs */
    case 0xe4: case 0xe5: case 0xe6: case 0xe7:	/* in/out imm */
    case 0xec: case 0xed: case 0xee: case 0xef:	/* in/out dx */
    case 0xcc: case 0xcd: case 0xce:		/* int3/int/into */
      return 1;

    case 0x0f:
      op = hist_peek_byte(emu, addr + 1, NULL);
      switch(op) {
        case 0x30:				/* wrmsr */
        case 0x31:				/* rdtsc */
        case 0x32:				/* rdmsr */
        case 0x33:				/* rdpmc */
        case 0xa2:				/* cpuid */
          return 1;
      }
      break;
  }

  return 0;
}


static hist_entry_t *hist_entry_push(hist_t *h)
{
  hist_entry_t *e;

  if(h->count == h->cap) {
    h->cap = h->cap ? 2 * h->cap : 16;
    h->entries = realloc(h->entries, h->cap * sizeof *h->entries);
  }

  e = &h->entries[h->count++];
  memset(e, 0, sizeof *e);
  e->last_pg = e->last_pt = ~0u;

  return e;
}


static void hist_entry_clear(hist_entry_t *e)
{
  free(e->bytes);
  free(e->hash);
  free(e->structs);
  memset(e, 0, sizeof *e);
}


/*
 * Undo all changes recorded in e. Memory bytes are restored in reverse
 * order directly on the host storage (never via the public write
 * interface, so no new access attributes are created); pages that were
 * materialized by the step become unmaterialized again; externally mapped
 * pages (x86emu_set_page) are restored in place.
 */
static void hist_rollback(x86emu_t *emu, hist_entry_t *e)
{
  x86emu_mem_t *mem = emu->mem;
  size_t u;

  for(u = e->bytes_n; u-- > 0; ) {
    *e->bytes[u].ptr = e->bytes[u].val;
  }

  for(u = e->structs_n; u-- > 0; ) {
    hrec_struct_t *r = e->structs + u;

    switch(r->kind) {
      case SREC_PAGE: {
        mem2_page_t *pg = &(*(*mem->pdir)[r->pdi])[r->pti];
        free(pg->attr);
        pg->attr = NULL;
        pg->data = NULL;
        pg->def_attr = r->def_attr;
        break;
      }

      case SREC_PTABLE:
        free((*mem->pdir)[r->pdi]);
        (*mem->pdir)[r->pdi] = NULL;
        break;

      case SREC_PDIR:
        free(mem->pdir);
        mem->pdir = NULL;
        break;
    }
  }

  emu->x86 = e->regs;
  if(emu->x86.msr) emu->x86.msr[0x10] = e->tsc;
  mem->invalid = e->mem_invalid;
}


API_SYM int x86emu_history_enable(x86emu_t *emu)
{
  hist_t *h;

  if(!emu || !emu->mem) return 1;

  /* custom i/o or code interception makes single steps unrecoverable */
  if(emu->memio != vm_memio || emu->code_check) return 1;

  if(emu->hist) x86emu_history_disable(emu);

  h = calloc(1, sizeof *h);
  if(!h) return 1;

  emu->hist = h;
  emu->memio = hist_memio;

  return 0;
}


API_SYM void x86emu_history_disable(x86emu_t *emu)
{
  hist_t *h;
  size_t u;

  if(!emu || !(h = emu->hist)) return;

  if(emu->memio == hist_memio) emu->memio = vm_memio;

  for(u = 0; u < h->count; u++) hist_entry_clear(h->entries + u);
  free(h->entries);
  free(h);

  emu->hist = NULL;
}


API_SYM unsigned x86emu_history_step(x86emu_t *emu)
{
  hist_t *h;
  hist_entry_t *e;
  u8 op1 = 0;
  unsigned rs;
  int has_prefix;

  if(!emu || !(h = emu->hist)) return X86EMU_HISTORY_DISABLED;

  /* a halted cpu stays halted until x86emu_history_back() */
  if(emu->x86.mode & _MODE_HALTED) return X86EMU_HISTORY_HLT;

  /* instructions that could observe or change the host are not executed */
  if(hist_barrier_ahead(emu)) return X86EMU_HISTORY_BARRIER;

  /* a pending interrupt would invoke the external intr handler */
  if(emu->x86.intr_type && emu->intr) return X86EMU_HISTORY_BARRIER;

  e = h->cur = hist_entry_push(h);

  /* fixed size cpu pre-image */
  e->regs = emu->x86;
  e->tsc = emu->x86.msr ? emu->x86.msr[0x10] : 0;
  e->mem_invalid = emu->mem->invalid;

  h->recording = 1;
  h->memerr = 0;
  h->barrier = 0;

  /* execute exactly one instruction; mirrors the x86emu_run() inner loop */
  *(emu->x86.disasm_ptr = emu->x86.disasm_buf) = 0;

  emu->x86.instr_len = 0;

  emu->x86.mode = 0;

  if(ACC_D(emu->x86.R_CS_ACC)) {
    emu->x86.mode |= _MODE_DATA32 | _MODE_ADDR32 | _MODE_CODE32;
  }
  if(ACC_D(emu->x86.R_SS_ACC)) {
    emu->x86.mode |= _MODE_STACK32;
  }

  emu->x86.default_seg = NULL;

  emu->x86.saved_cs = emu->x86.R_CS;
  emu->x86.saved_eip = emu->x86.R_EIP;

  memcpy(emu->x86.decode_seg, "[", 1);

  has_prefix = 1;
  while(has_prefix) {
    switch(op1 = fetch_byte(emu)) {
      case 0x26:
        memcpy(emu->x86.decode_seg, "es:[", 4);
        emu->x86.default_seg = emu->x86.seg + R_ES_INDEX;
        break;
      case 0x2e:
        memcpy(emu->x86.decode_seg, "cs:[", 4);
        emu->x86.default_seg = emu->x86.seg + R_CS_INDEX;
        break;
      case 0x36:
        memcpy(emu->x86.decode_seg, "ss:[", 4);
        emu->x86.default_seg = emu->x86.seg + R_SS_INDEX;
        break;
      case 0x3e:
        memcpy(emu->x86.decode_seg, "ds:[", 4);
        emu->x86.default_seg = emu->x86.seg + R_DS_INDEX;
        break;
      case 0x64:
        memcpy(emu->x86.decode_seg, "fs:[", 4);
        emu->x86.default_seg = emu->x86.seg + R_FS_INDEX;
        break;
      case 0x65:
        memcpy(emu->x86.decode_seg, "gs:[", 4);
        emu->x86.default_seg = emu->x86.seg + R_GS_INDEX;
        break;
      case 0x66:
        emu->x86.mode ^= _MODE_DATA32;
        break;
      case 0x67:
        emu->x86.mode ^= _MODE_ADDR32;
        break;
      case 0xf0:
        OP_DECODE("lock: ");
        break;
      case 0xf2:
        OP_DECODE("repne ");
        emu->x86.mode |= _MODE_REPNE;
        break;
      case 0xf3:
        OP_DECODE("repe ");
        emu->x86.mode |= _MODE_REPE;
        break;
      default:
        has_prefix = 0;
        break;
    }
  }

  /* a failed instruction fetch stops the cpu (and sets h->memerr) */
  if(!MODE_HALTED) {
    (*x86emu_optab[op1])(emu, op1);

    *emu->x86.disasm_ptr = 0;
  }

  h->recording = 0;

  if(h->memerr || h->barrier) {
    /* debug transaction failed: undo everything this step staged */
    rs = h->memerr ? X86EMU_HISTORY_MEMERR : X86EMU_HISTORY_BARRIER;
    hist_rollback(emu, e);
    hist_entry_clear(e);
    h->count--;
    h->cur = NULL;

    return rs;
  }

  if(emu->x86.intr_type && emu->intr) {
    /* a fault would invoke the external intr handler: roll back */
    hist_rollback(emu, e);
    hist_entry_clear(e);
    h->count--;
    h->cur = NULL;

    return X86EMU_HISTORY_BARRIER;
  }

  /* software vectoring without an external handler is ordinary state */
  handle_interrupt(emu);

  emu->x86.R_TSC++;	// time stamp counter

  h->cur = NULL;

  return MODE_HALTED ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK;
}


API_SYM unsigned x86emu_history_back(x86emu_t *emu)
{
  hist_t *h;
  hist_entry_t *e;

  if(!emu || !(h = emu->hist) || !h->count) return X86EMU_HISTORY_BACK_EMPTY;

  e = &h->entries[--h->count];

  hist_rollback(emu, e);
  hist_entry_clear(e);

  h->cur = NULL;

  return X86EMU_HISTORY_BACK_OK;
}


API_SYM void x86emu_history_stats(x86emu_t *emu, x86emu_history_stats_t *stats)
{
  hist_t *h;
  size_t u;

  if(!stats) return;

  memset(stats, 0, sizeof *stats);

  if(!emu || !(h = emu->hist)) return;

  stats->entries = h->count;

  for(u = 0; u < h->count; u++) {
    stats->data_bytes += h->entries[u].data_bytes;
    stats->attr_bytes += h->entries[u].attr_bytes;
  }
}
