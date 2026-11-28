/****************************************************************************
*
* Realmode X86 Emulator Library
*
* Description:
*   Contract tests for the reversible debugging (history) mode. Registered
*   through baseline/contract.h in the four mandated groups: instructions,
*   memory, barriers, branching. Every program is a small deterministic byte
*   sequence that is run forward, undone step by step and re-executed.
*
****************************************************************************/


#include "contract.h"
#include "x86emu.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CODE_BASE	0x1000u
#define WINDOW_END	0x40000u
#define WINDOW_LEN	(WINDOW_END)

static unsigned char snap_data[WINDOW_LEN];
static unsigned char snap_attr[WINDOW_LEN];
static x86emu_regs_t snap_regs;

static int cpuid_hits;
static int rdmsr_hits;
static int intr_hits;

static void cpuid_counter(x86emu_t *e) { (void) e; cpuid_hits++; }
static void rdmsr_counter(x86emu_t *e) { (void) e; rdmsr_hits++; }
static int intr_counter(x86emu_t *e, u8 n, unsigned t)
{
  (void) e; (void) n; (void) t; intr_hits++; return 0;
}
static int code_allow(x86emu_t *e) { (void) e; return 0; }
static unsigned memio_counter(x86emu_t *e, u32 a, u32 *v, unsigned t)
{
  (void) e; (void) a; (void) v; (void) t; return 0;
}


static x86emu_t *new_emu(unsigned def_perm)
{
  x86emu_t *e = x86emu_new(def_perm, 0);

  assert(e);
  x86emu_set_seg_register(e, e->x86.R_CS_SEL, 0);
  x86emu_set_seg_register(e, e->x86.R_DS_SEL, 0);
  x86emu_set_seg_register(e, e->x86.R_ES_SEL, 0);
  x86emu_set_seg_register(e, e->x86.R_SS_SEL, 0);
  e->x86.R_EIP = CODE_BASE;
  e->x86.R_ESP = 0x2000;
  e->x86.R_SP = 0x2000;

  return e;
}


/* switch the flat real mode machine to 32 bit code/stack/address size */
static void make32(x86emu_t *e)
{
  e->x86.R_CS_ACC |= 0x400;
  e->x86.R_SS_ACC |= 0x400;
  e->x86.R_CS_LIMIT = 0xffffffff;
  e->x86.R_SS_LIMIT = 0xffffffff;
  e->x86.R_DS_LIMIT = 0xffffffff;
  e->x86.R_ES_LIMIT = 0xffffffff;
}


static void load(x86emu_t *e, const unsigned char *code, unsigned len)
{
  unsigned u;

  for(u = 0; u < len; u++) x86emu_write_byte(e, CODE_BASE + u, code[u]);
  e->x86.R_EIP = CODE_BASE;
}


static void snapshot(x86emu_t *e)
{
  assert(x86emu_history_peek(e, 0, WINDOW_LEN, snap_data, snap_attr) ==
    WINDOW_LEN);
  snap_regs = e->x86;
}


static void assert_memory_restored(x86emu_t *e)
{
  static unsigned char data[WINDOW_LEN];
  static unsigned char attr[WINDOW_LEN];

  assert(x86emu_history_peek(e, 0, WINDOW_LEN, data, attr) == WINDOW_LEN);
  assert(memcmp(data, snap_data, WINDOW_LEN) == 0);
  assert(memcmp(attr, snap_attr, WINDOW_LEN) == 0);
}


static void assert_regs_restored(x86emu_t *e)
{
  /*
   * Compare only replayable CPU state. The timestamp counter (R_TSC) and wall
   * clock MSRs live behind the msr pointer and are intentionally not part of
   * the reversible state, so they are excluded by design.
   */
  assert(memcmp(&e->x86.gen, &snap_regs.gen, sizeof snap_regs.gen) == 0);
  assert(memcmp(&e->x86.spc, &snap_regs.spc, sizeof snap_regs.spc) == 0);
  assert(memcmp(e->x86.seg, snap_regs.seg, sizeof snap_regs.seg) == 0);
  assert(e->x86.mode == snap_regs.mode);
}


/* run to HLT, remembering how many steps it took */
static unsigned run_to_halt(x86emu_t *e)
{
  unsigned n = 0;
  int rc;

  for(;;) {
    rc = x86emu_history_step(e);
    assert(rc == X86EMU_HIST_OK || rc == X86EMU_HIST_HALTED);
    n++;
    if(rc == X86EMU_HIST_HALTED) break;
    assert(n < 1000);
  }

  return n;
}


static void undo_all(x86emu_t *e, unsigned n)
{
  unsigned u;

  for(u = 0; u < n; u++) {
    assert(x86emu_history_back(e) == X86EMU_HIST_BACK_OK);
  }
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_EMPTY);
  assert(x86emu_history_len(e) == 0);
  assert_regs_restored(e);
  assert_memory_restored(e);
}


/*----------------------------- instructions ------------------------------*/

static void t_arith_16(void)
{
  /* mov ax,0x1234; mov bx,0x3210; add ax,bx; mov [0x3000],ax; hlt */
  const unsigned char code[] = {
    0xb8, 0x34, 0x12,
    0xbb, 0x10, 0x32,
    0x01, 0xd8,
    0xa3, 0x00, 0x30,
    0xf4
  };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned n;

  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  n = run_to_halt(e);
  assert(e->x86.R_AX == 0x4444);
  {
    unsigned char w[2];
    x86emu_history_peek(e, 0x3000, 2, w, NULL);
    assert(w[0] == 0x44 && w[1] == 0x44);
  }

  undo_all(e, n);
  n = run_to_halt(e);			/* deterministic re-execution */
  assert(e->x86.R_AX == 0x4444);
  undo_all(e, n);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_call_ret_stack_32(void)
{
  /*
   * 32 bit: mov eax,0; call the inc below; nop; hlt; inc eax; ret;
   * offsets: call next = 0x0a, inc target = 0x0c -> rel32 = 2
   */
  const unsigned char code[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00,	/* 00: mov eax, 0      */
    0xe8, 0x02, 0x00, 0x00, 0x00,	/* 05: call +2 -> 0c   */
    0x90,				/* 0a: nop             */
    0xf4,				/* 0b: hlt             */
    0xff, 0xc0,				/* 0c: inc eax         */
    0xc3				/* 0e: ret             */
  };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned n;

  make32(e);
  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  n = run_to_halt(e);
  assert(e->x86.R_EAX == 1);
  assert(e->x86.R_ESP == 0x2000);	/* call/ret balanced */

  undo_all(e, n);
  n = run_to_halt(e);
  assert(e->x86.R_EAX == 1 && e->x86.R_ESP == 0x2000);
  undo_all(e, n);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_string_ops(void)
{
  /* 16 bit rep movsb, rep stosb and repe cmpsb; hlt */
  const unsigned char code[] = {
    0xf3, 0xa4,				/* rep movsb           */
    0xb0, 0x7a,				/* mov al, 'z'         */
    0xb9, 0x04, 0x00,			/* mov cx, 4           */
    0xf3, 0xaa,				/* rep stosb 0x5004..  */
    0xbe, 0x00, 0x40,			/* mov si, 0x4000      */
    0xbf, 0x00, 0x50,			/* mov di, 0x5000      */
    0xb9, 0x04, 0x00,			/* mov cx, 4           */
    0xf3, 0xa6,				/* repe cmpsb          */
    0xf4
  };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned n, u;

  for(u = 0; u < 4; u++) x86emu_write_byte(e, 0x4000 + u, 0x10 + u);
  load(e, code, sizeof code);
  e->x86.R_SI = 0x4000;
  e->x86.R_DI = 0x5000;
  e->x86.R_CX = 4;

  assert(x86emu_history_enable(e));
  snapshot(e);

  n = run_to_halt(e);
  /* movsb landed 0x10..0x13 at 0x5000; stosb then filled 0x5004..0x5007 */
  for(u = 0; u < 4; u++) {
    unsigned char b;
    x86emu_history_peek(e, 0x5000 + u, 1, &b, NULL);
    assert(b == (unsigned char) (0x10 + u));
    x86emu_history_peek(e, 0x5004 + u, 1, &b, NULL);
    assert(b == 0x7a);
  }
  /* cmpsb compared four equal bytes -> zero flag stays set, cx exhausted */
  assert(e->x86.R_CX == 0);

  undo_all(e, n);
  {
    unsigned char b;
    x86emu_history_peek(e, 0x5000, 1, &b, NULL);
    assert(b == 0xff);		/* destination page back to pristine */
  }
  assert(e->x86.R_CX == 4 && e->x86.R_SI == 0x4000 && e->x86.R_DI == 0x5000);

  n = run_to_halt(e);
  undo_all(e, n);

  x86emu_history_disable(e);
  x86emu_done(e);
}


/*-------------------------------- memory ---------------------------------*/

static void t_rep_cross_page_fault(void)
{
  /* nop; rep stosb: 16 bytes land in the writable page, then hit a RO page */
  const unsigned char code[] = { 0x90, 0xf3, 0xaa, 0xf4 };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned before;
  int rc;

  x86emu_set_perm(e, 0x2000, 0x2fff, X86EMU_PERM_RX);	/* no write */
  load(e, code, sizeof code);
  e->x86.R_DI = 0x1ff0;
  e->x86.R_CX = 0x20;
  e->x86.R_AL = 0x5a;

  assert(x86emu_history_enable(e));
  /* a successful step first: it must stay undoable after the failed step */
  assert(x86emu_history_step(e) == X86EMU_HIST_OK);
  before = x86emu_history_len(e);
  snapshot(e);

  rc = x86emu_history_step(e);
  assert(rc == X86EMU_HIST_MEMFAULT);
  assert(x86emu_history_len(e) == before);	/* nothing published */

  /* whole instruction rolled back: registers, bytes and attributes */
  assert(e->x86.R_DI == 0x1ff0 && e->x86.R_CX == 0x20);
  assert_regs_restored(e);
  assert_memory_restored(e);

  /* the earlier successful step can still be undone */
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_OK);
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_EMPTY);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_external_alias(void)
{
  /*
   * Two guest pages alias one host buffer. An overlapping rep movsb inside the
   * same storage rewrites several bytes; undo must restore every original byte
   * in place and must not replace the external mapping with a library copy.
   */
  const unsigned char code[] = { 0xf3, 0xa4, 0xf4 };
  unsigned char *host = malloc(X86EMU_PAGE_SIZE);
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned n, u;

  assert(host);
  for(u = 0; u < 8; u++) host[u] = (unsigned char) u;
  x86emu_set_page(e, 0x10000, host);
  x86emu_set_page(e, 0x20000, host);

  make32(e);
  load(e, code, sizeof code);
  e->x86.R_ESI = 0x10000;		/* host + 0 */
  e->x86.R_EDI = 0x20002;		/* host + 2 (overlapping) */
  e->x86.R_ECX = 4;

  assert(x86emu_history_enable(e));
  snapshot(e);

  n = run_to_halt(e);
  /* the overlapping aliased move really did overwrite shared host storage */
  assert(host[2] == 0 && host[3] == 1);

  undo_all(e, n);
  /* first touch pre-images restore the true pre-instruction host contents */
  for(u = 0; u < 8; u++) assert(host[u] == (unsigned char) u);
  {
    unsigned char via_alias[8];
    x86emu_history_peek(e, 0x20000, 8, via_alias, NULL);
    for(u = 0; u < 8; u++) assert(via_alias[u] == (unsigned char) u);
  }

  x86emu_history_disable(e);
  x86emu_done(e);
  free(host);
}


static void t_peek_sparse_stats(void)
{
  /* mov byte [0x30000], 0x41 (32 bit addr) ; hlt */
  const unsigned char code[] = {
    0xc6, 0x05, 0x00, 0x00, 0x03, 0x00, 0x41, 0xf4
  };
  /* two independent one byte writes to 0x10000 / 0x10001, then hlt */
  const unsigned char w[] = {
    0xc6, 0x05, 0x00, 0x00, 0x01, 0x00, 0x77,
    0xc6, 0x05, 0x01, 0x00, 0x01, 0x00, 0x88,
    0xf4
  };
  unsigned char *big = malloc(1u << 20);
  x86emu_history_stats_t st;
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned char d;
  unsigned char a;
  unsigned n;
  unsigned u;

  assert(big);
  memset(big, 0, 1u << 20);
  make32(e);
  /* map an extra 1 MiB (256 pages) that is never accessed */
  for(u = 0; u < 256; u++) {
    x86emu_set_page(e, 0x80000 + u * X86EMU_PAGE_SIZE,
      big + u * X86EMU_PAGE_SIZE);
  }

  /* phase 1: a single byte write materializing one sparse page */
  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  assert(x86emu_history_peek(e, 0x30000, 1, &d, &a) == 1);
  assert(d == 0xff && a == X86EMU_PERM_RWX);	/* 0xff + default attr */
  assert(x86emu_history_len(e) == 0);

  n = run_to_halt(e);
  assert(x86emu_history_get_stats(e, &st) == 0);
  assert(st.entries == n);
  assert(st.page_events >= 1);			/* sparse page materialized */

  undo_all(e, x86emu_history_len(e));
  assert(x86emu_history_peek(e, 0x30000, 1, &d, &a) == 1);
  assert(d == 0xff && a == X86EMU_PERM_RWX);	/* page unmaterialized again */

  /*
   * phase 2: the never accessed extra 1 MiB must not grow the pre-images. Run
   * the same two-byte-write program on an emu with the mapping and one without
   * and require identical retained byte counts.
   */
  {
    x86emu_history_stats_t a, b;
    x86emu_t *small = new_emu(X86EMU_PERM_RWX);
    x86emu_t *big_emu = new_emu(X86EMU_PERM_RWX);

    make32(small);
    make32(big_emu);
    for(u = 0; u < 256; u++) {
      x86emu_set_page(big_emu, 0x80000 + u * X86EMU_PAGE_SIZE,
        big + u * X86EMU_PAGE_SIZE);
    }

    load(small, w, sizeof w);
    load(big_emu, w, sizeof w);
    assert(x86emu_history_enable(small));
    assert(x86emu_history_enable(big_emu));

    assert(x86emu_history_step(small) == X86EMU_HIST_OK);
    assert(x86emu_history_step(big_emu) == X86EMU_HIST_OK);
    assert(x86emu_history_step(small) == X86EMU_HIST_OK);
    assert(x86emu_history_step(big_emu) == X86EMU_HIST_OK);

    assert(x86emu_history_get_stats(small, &a) == 0);
    assert(x86emu_history_get_stats(big_emu, &b) == 0);
    /* the two data writes are included in the small, touched-only footprint */
    assert(a.data_bytes >= 2 && b.data_bytes >= 2);
    assert(a.data_bytes == b.data_bytes);
    assert(a.attr_bytes == b.attr_bytes);		/* 1 MiB adds nothing */
    /* retained bytes stay tiny: no whole page was copied */
    assert(b.data_bytes < X86EMU_PAGE_SIZE);
    assert(b.attr_bytes < X86EMU_PAGE_SIZE);
    assert(b.data_bytes + b.attr_bytes < (1u << 20));

    x86emu_history_disable(small);
    x86emu_history_disable(big_emu);
    x86emu_done(small);
    x86emu_done(big_emu);
  }

  x86emu_history_disable(e);
  x86emu_done(e);
  free(big);
}


/*------------------------------- barriers --------------------------------*/

static void t_hlt_back(void)
{
  const unsigned char code[] = { 0xb8, 0x05, 0x00, 0xf4 };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  x86emu_regs_t at_hlt;

  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  assert(x86emu_history_step(e) == X86EMU_HIST_OK);
  assert(x86emu_history_step(e) == X86EMU_HIST_HALTED);
  assert(e->x86.mode & _MODE_HALTED);
  at_hlt = e->x86;

  /* HLT is undoable; the halted state is restored on re-execution */
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_OK);
  assert(!(e->x86.mode & _MODE_HALTED));
  assert(e->x86.R_EIP == CODE_BASE + 3);
  assert(x86emu_history_step(e) == X86EMU_HIST_HALTED);
  /* identical replayable register state and the same stopped (halted) state */
  assert(memcmp(&e->x86.gen, &at_hlt.gen, sizeof at_hlt.gen) == 0);
  assert(memcmp(&e->x86.spc, &at_hlt.spc, sizeof at_hlt.spc) == 0);
  assert(memcmp(e->x86.seg, at_hlt.seg, sizeof at_hlt.seg) == 0);
  /* identical mode, including the stopped (halted) state */
  assert(e->x86.mode == at_hlt.mode);

  /* rewind both re-executed steps; the very last back must report empty */
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_OK);
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_OK);
  assert(x86emu_history_back(e) == X86EMU_HIST_BACK_EMPTY);
  assert_regs_restored(e);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_io_barrier_no_host(void)
{
  /* in al,0x40 ; out 0x40,al ; hlt */
  const unsigned char code[] = { 0xe4, 0x40, 0xe6, 0x40, 0xf4 };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);

  x86emu_set_io_perm(e, 0x40, 0x40, X86EMU_PERM_RW);
  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  assert(x86emu_history_step(e) == X86EMU_HIST_BARRIER);
  assert(e->io.stats_i[0x40] == 0);		/* no host input happened */
  assert_regs_restored(e);
  assert_memory_restored(e);

  /* skip the IN and try the OUT; re-snapshot the new boundary state */
  e->x86.R_EIP = CODE_BASE + 2;
  snapshot(e);
  assert(x86emu_history_step(e) == X86EMU_HIST_BARRIER);
  assert(e->io.stats_o[0x40] == 0);		/* no host output happened */
  assert(e->x86.R_EIP == CODE_BASE + 2);	/* step did not advance EIP */
  assert_regs_restored(e);
  assert_memory_restored(e);

  assert(x86emu_history_len(e) == 0);
  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_callback_barriers(void)
{
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);

  x86emu_set_cpuid_handler(e, cpuid_counter);
  x86emu_set_rdmsr_handler(e, rdmsr_counter);
  x86emu_set_intr_handler(e, intr_counter);

  /* cpuid */
  load(e, (const unsigned char[]) { 0x0f, 0xa2, 0xf4 }, 3);
  assert(x86emu_history_enable(e));
  snapshot(e);
  assert(x86emu_history_step(e) == X86EMU_HIST_BARRIER);
  assert(cpuid_hits == 0);
  assert_regs_restored(e);

  /* rdmsr */
  load(e, (const unsigned char[]) { 0x0f, 0x32, 0xf4 }, 3);
  snapshot(e);
  assert(x86emu_history_step(e) == X86EMU_HIST_BARRIER);
  assert(rdmsr_hits == 0);
  assert_regs_restored(e);

  /* int 0x20 */
  load(e, (const unsigned char[]) { 0xcd, 0x20, 0xf4 }, 3);
  snapshot(e);
  assert(x86emu_history_step(e) == X86EMU_HIST_BARRIER);
  assert(intr_hits == 0);
  assert_regs_restored(e);
  assert_memory_restored(e);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_enable_rejects_handlers(void)
{
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  x86emu_memio_handler_t old_memio;

  old_memio = x86emu_set_memio_handler(e, memio_counter);
  assert(x86emu_history_enable(e) == 0);
  x86emu_set_memio_handler(e, old_memio);

  assert(x86emu_history_enable(e) == 1);
  x86emu_history_disable(e);

  x86emu_set_code_handler(e, code_allow);
  assert(x86emu_history_enable(e) == 0);

  x86emu_done(e);
}


/*------------------------------ branching --------------------------------*/

static void t_cond_jump_16(void)
{
  /* mov ax,1; cmp ax,1; jz +2; inc ax; inc ax; hlt (jump taken) */
  const unsigned char code[] = {
    0xb8, 0x01, 0x00,
    0x3d, 0x01, 0x00,
    0x74, 0x02,
    0x40,
    0x40,
    0xf4
  };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned n;

  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  n = run_to_halt(e);
  assert(e->x86.R_AX == 1);			/* both inc skipped */
  undo_all(e, n);

  /* not taken: edit a general register at the boundary and take the fork */
  assert(x86emu_history_step(e) == X86EMU_HIST_OK);	/* mov ax,1 */
  e->x86.R_AX = 0;				/* boundary register edit */
  n = run_to_halt(e);
  assert(e->x86.R_AX == 2);			/* both inc executed */
  undo_all(e, x86emu_history_len(e));
  assert_regs_restored(e);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_loop_diverges(void)
{
  /* dec cx; cmp cl,0; jnz back to dec; hlt -- count supplied in CX */
  const unsigned char code[] = {
    0x49,				/* dec cx             */
    0x80, 0xf9, 0x00,			/* cmp cl, 0          */
    0x75, 0xfa,				/* jnz -6 -> dec      */
    0xf4
  };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned long first;

  load(e, code, sizeof code);
  e->x86.R_CX = 3;
  assert(x86emu_history_enable(e));
  snapshot(e);

  first = run_to_halt(e);
  assert(e->x86.R_CX == 0);

  /* rewind and start a new branch with a different loop count */
  undo_all(e, (unsigned) first);
  assert(x86emu_history_len(e) == 0);		/* old future is gone */
  e->x86.R_CX = 1;
  snapshot(e);				/* boundary of the new branch */
  {
    unsigned long second = run_to_halt(e);
    assert(second < first);			/* new, shorter branch */
    assert(e->x86.R_CX == 0);
    undo_all(e, (unsigned) second);
  }
  assert_regs_restored(e);

  x86emu_history_disable(e);
  x86emu_done(e);
}


static void t_branch_32(void)
{
  /* 32 bit near call through an indirect register and ret, with push/pop */
  const unsigned char code[] = {
    0x68, 0x77, 0x00, 0x00, 0x00,	/* push 0x77          */
    0x58,				/* pop eax            */
    0xe8, 0x02, 0x00, 0x00, 0x00,	/* call +2 -> 0d dec  */
    0x90,				/* nop                */
    0xf4,				/* hlt                */
    0x48,				/* dec eax (32 bit)   */
    0xc3				/* ret                */
  };
  x86emu_t *e = new_emu(X86EMU_PERM_RWX);
  unsigned n;

  make32(e);
  load(e, code, sizeof code);
  assert(x86emu_history_enable(e));
  snapshot(e);

  n = run_to_halt(e);
  assert(e->x86.R_EAX == 0x76);
  assert(e->x86.R_ESP == 0x2000);
  undo_all(e, n);
  n = run_to_halt(e);
  assert(e->x86.R_EAX == 0x76 && e->x86.R_ESP == 0x2000);
  undo_all(e, n);

  x86emu_history_disable(e);

  /* after disabling, plain x86emu_run keeps its original behaviour */
  load(e, (const unsigned char[]) { 0xb8, 0x77, 0x00, 0x00, 0x00, 0xf4 }, 6);
  e->x86.R_EAX = 0;
  x86emu_run(e, X86EMU_RUN_MAX_INSTR);
  assert(e->x86.R_EAX == 0x77);

  x86emu_done(e);
}


void register_contract(void)
{
  contract_add("instructions", "arith_16", t_arith_16);
  contract_add("instructions", "call_ret_stack_32", t_call_ret_stack_32);
  contract_add("instructions", "string_movs_stos_cmps", t_string_ops);

  contract_add("memory", "rep_cross_page_fault", t_rep_cross_page_fault);
  contract_add("memory", "external_alias", t_external_alias);
  contract_add("memory", "peek_sparse_stats", t_peek_sparse_stats);

  contract_add("barriers", "hlt_back", t_hlt_back);
  contract_add("barriers", "io_barrier_no_host", t_io_barrier_no_host);
  contract_add("barriers", "callback_barriers", t_callback_barriers);
  contract_add("barriers", "enable_rejects_handlers", t_enable_rejects_handlers);

  contract_add("branching", "cond_jump_16", t_cond_jump_16);
  contract_add("branching", "loop_diverges", t_loop_diverges);
  contract_add("branching", "call_stack_32", t_branch_32);
}
