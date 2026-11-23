/*
 * Contract tests for the reversible debugging (history) interface.
 *
 * Registered via baseline/contract.h; driven by baseline/driver.c.
 */

#include "x86emu.h"
#include "contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *file, int line, const char *expr)
{
  printf("FAIL %s:%d: %s\n", file, line, expr);
  exit(1);
}

#define CHECK(cond) do { if(!(cond)) fail(__FILE__, __LINE__, #cond); } while(0)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static x86emu_t *mk_emu(void)
{
  x86emu_t *e = x86emu_new(X86EMU_PERM_RWX, 0);

  CHECK(e != NULL);

  x86emu_set_seg_register(e, e->x86.R_CS_SEL, 0);
  x86emu_set_seg_register(e, e->x86.R_DS_SEL, 0);
  x86emu_set_seg_register(e, e->x86.R_ES_SEL, 0);
  x86emu_set_seg_register(e, e->x86.R_SS_SEL, 0);

  e->x86.R_EIP = 0x100;
  e->x86.R_SP = 0x8000;

  return e;
}

static void load(x86emu_t *e, u32 addr, const u8 *code, unsigned len)
{
  unsigned u;

  for(u = 0; u < len; u++) x86emu_write_byte(e, addr + u, code[u]);
}

static u8 peek8(x86emu_t *e, u32 addr)
{
  u8 d;

  x86emu_history_peek(e, addr, 1, &d, NULL);

  return d;
}

static u8 peek_attr(x86emu_t *e, u32 addr)
{
  u8 a;

  x86emu_history_peek(e, addr, 1, NULL, &a);

  return a;
}

static int page_materialized(x86emu_t *e, u32 addr)
{
  mem2_ptable_t *pt;

  if(!e->mem->pdir) return 0;

  pt = (*e->mem->pdir)[addr >> (32 - X86EMU_PDIR_BITS)];
  if(!pt) return 0;

  return (*pt)[(addr >> X86EMU_PAGE_BITS) & ((1 << X86EMU_PTABLE_BITS) - 1)].attr != NULL;
}

typedef struct {
  u32 eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflg, mode;
  u8 mem[64];
} snap_t;

static void snap_take(x86emu_t *e, snap_t *s, u32 mem_addr)
{
  s->eax = e->x86.R_EAX;
  s->ebx = e->x86.R_EBX;
  s->ecx = e->x86.R_ECX;
  s->edx = e->x86.R_EDX;
  s->esi = e->x86.R_ESI;
  s->edi = e->x86.R_EDI;
  s->ebp = e->x86.R_EBP;
  s->esp = e->x86.R_ESP;
  s->eip = e->x86.R_EIP;
  s->eflg = e->x86.R_EFLG;
  s->mode = e->x86.mode;
  x86emu_history_peek(e, mem_addr, sizeof s->mem, s->mem, NULL);
}

static void snap_check(const snap_t *a, const snap_t *b)
{
  CHECK(!memcmp(a, b, sizeof *a));
}

/* ------------------------------------------------------------------ */
/* group: instructions                                                 */
/* ------------------------------------------------------------------ */

/*
 * 16 bit arithmetic, stack and near call/ret: step a small program to
 * completion, undo it step by step comparing full state snapshots, then
 * re-execute and expect identical states again.
 */
static void t_instructions_alu_stack_call_ret_16(void)
{
  static const u8 code[] = {
    0xb8, 0x34, 0x12,       /* 0x100: mov ax,0x1234 */
    0x05, 0x01, 0x00,       /* 0x103: add ax,1 */
    0x50,                   /* 0x106: push ax */
    0x5b,                   /* 0x107: pop bx */
    0xe8, 0x01, 0x00,       /* 0x108: call 0x10c */
    0xf4,                   /* 0x10b: hlt */
    0xc3                    /* 0x10c: ret */
  };
  enum { STEPS = 7 };
  x86emu_t *e = mk_emu();
  snap_t fwd[STEPS + 1];
  unsigned u, rs;

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);

  for(u = 0; u < STEPS; u++) {
    snap_take(e, &fwd[u], 0x7ff0);
    rs = x86emu_history_step(e);
    CHECK(rs == (u == STEPS - 1 ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK));
  }
  snap_take(e, &fwd[STEPS], 0x7ff0);

  CHECK(e->x86.R_AX == 0x1235);
  CHECK(e->x86.R_BX == 0x1235);
  CHECK(e->x86.R_SP == 0x8000);
  CHECK(!(e->x86.R_EFLG & F_ZF));       /* 0x1234 + 1 != 0 */
  CHECK(e->x86.mode & 0x80);          /* _MODE_HALTED */

  /* undo everything, comparing against the forward snapshots */
  for(u = STEPS; u-- > 0; ) {
    CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
    {
      snap_t now;
      snap_take(e, &now, 0x7ff0);
      snap_check(&now, &fwd[u]);
    }
  }
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  /* re-execute: identical states again */
  for(u = 0; u < STEPS; u++) {
    snap_t now;
    rs = x86emu_history_step(e);
    CHECK(rs == (u == STEPS - 1 ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK));
    snap_take(e, &now, 0x7ff0);
    snap_check(&now, &fwd[u + 1]);
  }

  x86emu_history_disable(e);
  x86emu_done(e);
}

/* REP string instructions (MOVS/STOS/CMPS) count as single steps. */
static void t_instructions_string_ops_16(void)
{
  static const u8 code[] = {
    0xb9, 0x03, 0x00,       /* 0x100: mov cx,3 */
    0xbe, 0x00, 0x03,       /* 0x103: mov si,0x300 */
    0xbf, 0x00, 0x04,       /* 0x106: mov di,0x400 */
    0xf3, 0xa4,             /* 0x109: rep movsb */
    0xb0, 0x5a,             /* 0x10b: mov al,0x5a */
    0xb9, 0x02, 0x00,       /* 0x10d: mov cx,2 */
    0xf3, 0xaa,             /* 0x110: rep stosb */
    0xbe, 0x00, 0x03,       /* 0x112: mov si,0x300 */
    0xbf, 0x00, 0x04,       /* 0x115: mov di,0x400 */
    0xb9, 0x03, 0x00,       /* 0x118: mov cx,3 */
    0xf3, 0xa6,             /* 0x11b: repe cmpsb */
    0xf4                    /* 0x11d: hlt */
  };
  enum { STEPS = 12 };
  x86emu_t *e = mk_emu();
  snap_t fwd[STEPS + 1];
  unsigned u, rs;

  load(e, 0x100, code, sizeof code);
  load(e, 0x300, (const u8 []) { 1, 2, 3 }, 3);

  CHECK(x86emu_history_enable(e) == 0);

  for(u = 0; u < STEPS; u++) {
    snap_take(e, &fwd[u], 0x3f0);
    rs = x86emu_history_step(e);
    CHECK(rs == (u == STEPS - 1 ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK));
    if(u == 3) {
      /* the whole rep movsb was a single step */
      CHECK(e->x86.R_CX == 0);
      CHECK(peek8(e, 0x400) == 1 && peek8(e, 0x401) == 2 && peek8(e, 0x402) == 3);
    }
  }
  snap_take(e, &fwd[STEPS], 0x3f0);

  CHECK(peek8(e, 0x403) == 0x5a && peek8(e, 0x404) == 0x5a);
  CHECK(e->x86.R_CX == 0);
  CHECK(e->x86.R_EFLG & F_ZF);          /* repe cmpsb found all bytes equal */

  for(u = STEPS; u-- > 0; ) {
    CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
    {
      snap_t now;
      snap_take(e, &now, 0x3f0);
      snap_check(&now, &fwd[u]);
    }
  }
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  for(u = 0; u < STEPS; u++) {
    snap_t now;
    rs = x86emu_history_step(e);
    CHECK(rs == (u == STEPS - 1 ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK));
    snap_take(e, &now, 0x3f0);
    snap_check(&now, &fwd[u + 1]);
  }

  x86emu_history_disable(e);
  x86emu_done(e);
}

/* 32 bit mode: 32 bit arithmetic, stack and near call. */
static void t_instructions_mode32(void)
{
  static const u8 code[] = {
    0xb8, 0x78, 0x56, 0x34, 0x12,   /* 0x100: mov eax,0x12345678 */
    0x83, 0xc0, 0x01,               /* 0x105: add eax,1 */
    0x50,                           /* 0x108: push eax */
    0x5b,                           /* 0x109: pop ebx */
    0xe8, 0x00, 0x00, 0x00, 0x00,   /* 0x10a: call 0x10f */
    0x59,                           /* 0x10f: pop ecx */
    0xf4                            /* 0x110: hlt */
  };
  enum { STEPS = 7 };
  x86emu_t *e = mk_emu();
  snap_t fwd[STEPS + 1];
  unsigned u, rs;

  e->x86.R_CS_ACC = 0x9b | (1 << 10);   /* 32 bit code segment */
  e->x86.R_SS_ACC = 0x93 | (1 << 10);   /* 32 bit stack segment */
  e->x86.R_CS_LIMIT = 0xffffffff;
  e->x86.R_SS_LIMIT = 0xffffffff;
  e->x86.R_DS_LIMIT = 0xffffffff;
  e->x86.R_ES_LIMIT = 0xffffffff;
  e->x86.R_ESP = 0x9000;

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);

  for(u = 0; u < STEPS; u++) {
    snap_take(e, &fwd[u], 0x8ff0);
    rs = x86emu_history_step(e);
    CHECK(rs == (u == STEPS - 1 ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK));
  }
  snap_take(e, &fwd[STEPS], 0x8ff0);

  CHECK(e->x86.R_EAX == 0x12345679);
  CHECK(e->x86.R_EBX == 0x12345679);
  CHECK(e->x86.R_ECX == 0x10f);
  CHECK(e->x86.R_ESP == 0x9000);

  for(u = STEPS; u-- > 0; ) {
    CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
    {
      snap_t now;
      snap_take(e, &now, 0x8ff0);
      snap_check(&now, &fwd[u]);
    }
  }
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  for(u = 0; u < STEPS; u++) {
    snap_t now;
    rs = x86emu_history_step(e);
    CHECK(rs == (u == STEPS - 1 ? X86EMU_HISTORY_HLT : X86EMU_HISTORY_OK));
    snap_take(e, &now, 0x8ff0);
    snap_check(&now, &fwd[u + 1]);
  }

  x86emu_history_disable(e);
  x86emu_done(e);
}

/* After x86emu_history_disable() a plain x86emu_run() behaves as before. */
static void t_instructions_disable_then_normal_run(void)
{
  static const u8 code[] = {
    0xb8, 0x34, 0x12,       /* 0x100: mov ax,0x1234 */
    0xa3, 0x00, 0x20,       /* 0x103: mov [0x2000],ax */
    0xf4                    /* 0x106: hlt */
  };
  x86emu_t *e = mk_emu();
  unsigned rs;

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(e->x86.R_AX == 0x1234);

  x86emu_history_disable(e);

  /* plain run continues from the current eip and honors max_instr */
  e->max_instr = 100;
  rs = x86emu_run(e, X86EMU_RUN_MAX_INSTR);
  CHECK(!(rs & X86EMU_RUN_MAX_INSTR));
  CHECK(e->x86.R_AX == 0x1234);
  CHECK(x86emu_read_word(e, 0x2000) == 0x1234);

  x86emu_done(e);
}

/* ------------------------------------------------------------------ */
/* group: memory                                                       */
/* ------------------------------------------------------------------ */

/*
 * A REP movsb crossing into a non-writable page fails as a debug
 * transaction: registers, already written bytes, VALID/access bits and
 * the invalid flag are all rolled back and nothing is published to the
 * history. Earlier steps can still be undone.
 */
static void t_memory_rep_cross_page_failure(void)
{
  static const u8 code[] = {
    0xb9, 0x20, 0x00,       /* 0x100: mov cx,0x20 */
    0xbe, 0x00, 0x10,       /* 0x103: mov si,0x1000 */
    0xbf, 0xf0, 0x2f,       /* 0x106: mov di,0x2ff0 */
    0xf3, 0xa4,             /* 0x109: rep movsb */
    0xf4                    /* 0x10b: hlt */
  };
  x86emu_t *e = mk_emu();
  x86emu_history_stats_t st;
  unsigned u;
  u32 eip;

  for(u = 0; u < 0x10; u++) x86emu_write_byte(e, 0x1000 + u, u);

  /* the page after the destination window is not writable */
  x86emu_set_perm(e, 0x3000, 0x3fff, X86EMU_PERM_RX);

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);

  /* rep movsb writes 0x2ff0.. and hits the read-only page at 0x3000 */
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_MEMERR);

  /* the whole step was rolled back */
  CHECK(e->x86.R_CX == 0x20);
  CHECK(e->x86.R_SI == 0x1000);
  CHECK(e->x86.R_DI == 0x2ff0);
  CHECK(e->x86.R_EIP == 0x109);
  CHECK(e->mem->invalid == 0);

  /* the sparse destination page is unmaterialized again: no data, no
   * VALID/access bits were leaked */
  CHECK(!page_materialized(e, 0x2ff0));
  CHECK(peek8(e, 0x2ff0) == 0xff);
  CHECK(peek_attr(e, 0x2ff0) == X86EMU_PERM_RWX);
  CHECK(peek_attr(e, 0x3000) == X86EMU_PERM_RX);

  /* the failed step was not published */
  x86emu_history_stats(e, &st);
  CHECK(st.entries == 3);

  /* the failure is deterministic */
  eip = e->x86.R_EIP;
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_MEMERR);
  CHECK(e->x86.R_EIP == eip);

  /* earlier steps still undo cleanly */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(e->x86.R_DI == 0);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  /* redirect the copy to a writable window: registers may be changed at
   * an instruction boundary */
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  e->x86.R_DI = 0x2000;
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(e->x86.R_CX == 0);
  for(u = 0; u < 0x10; u++) CHECK(peek8(e, 0x2000 + u) == u);
  CHECK(peek_attr(e, 0x2000) & X86EMU_PERM_VALID);
  CHECK(peek_attr(e, 0x2000) & X86EMU_ACC_W);

  /* undoing the rep step restores the unmaterialized page */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(!page_materialized(e, 0x2000));
  CHECK(peek8(e, 0x2000) == 0xff);

  x86emu_history_disable(e);
  x86emu_done(e);
}

/*
 * Two guest pages mapped to the same host storage via x86emu_set_page:
 * undo restores the host bytes in place (no library-allocated copy) and
 * aliased accesses keep the true pre-instruction content.
 */
static void t_memory_external_alias(void)
{
  static u8 host[X86EMU_PAGE_SIZE];
  static const u8 code[] = {
    0xc7, 0x06, 0x00, 0x50, 0xef, 0xbe,   /* 0x100: mov word [0x5000],0xbeef */
    0xa1, 0x00, 0x60,                     /* 0x106: mov ax,[0x6000] */
    0xb9, 0x04, 0x00,                     /* 0x109: mov cx,4 */
    0xbe, 0x04, 0x50,                     /* 0x10c: mov si,0x5004 */
    0xbf, 0x00, 0x60,                     /* 0x10f: mov di,0x6000 */
    0xf3, 0xa4,                           /* 0x112: rep movsb */
    0xf4                                  /* 0x114: hlt */
  };
  x86emu_t *e;
  unsigned u;

  for(u = 0; u < sizeof host; u++) host[u] = (u8) (u ^ 0x5a);

  e = mk_emu();

  /* two guest pages, one host storage */
  x86emu_set_page(e, 0x5000, host);
  x86emu_set_page(e, 0x6000, host);

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(host[0] == 0xef && host[1] == 0xbe);          /* written in place */
  CHECK(peek8(e, 0x6000) == 0xef);                    /* visible via the alias */

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(e->x86.R_AX == 0xbeef);                       /* read via the alias */

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);

  /* rep movsb copies host[4..7] onto host[0..3] through the alias */
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  for(u = 0; u < 4; u++) CHECK(host[u] == (u8) ((u + 4) ^ 0x5a));

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_HLT);

  /* undo everything: the original host content comes back, in place */
  for(u = 0; u < 7; u++) CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  for(u = 0; u < sizeof host; u++) CHECK(host[u] == (u8) (u ^ 0x5a));
  CHECK(peek8(e, 0x5000) == (0 ^ 0x5a));
  CHECK(peek8(e, 0x6000) == (0 ^ 0x5a));
  CHECK(peek_attr(e, 0x5000) == (X86EMU_PERM_RWX | X86EMU_PERM_VALID));
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  x86emu_history_disable(e);
  x86emu_done(e);
}

/*
 * Statistics and peek: pre-images stay sparse. Mapping an extra 1 MiB of
 * memory that is never accessed must not grow the recorded pre-image of a
 * two byte write.
 */
static void t_memory_peek_and_stats(void)
{
  static u8 big[1 << 20];
  static const u8 code[] = {
    0xc7, 0x06, 0x00, 0x20, 0x34, 0x12,   /* 0x100: mov word [0x2000],0x1234 */
    0xf4                                  /* 0x106: hlt */
  };
  x86emu_t *e = mk_emu();
  x86emu_history_stats_t st;
  u32 p;
  u8 d, a;

  /* map 1 MiB of external memory that is never accessed */
  for(p = 0; p < (1 << 20); p += X86EMU_PAGE_SIZE) {
    x86emu_set_page(e, 0x100000 + p, big + p);
  }

  load(e, 0x100, code, sizeof code);

  /* materialize the destination page up front, so the two byte write
   * below records a sparse two byte pre-image */
  x86emu_write_byte(e, 0x2000, 0);

  CHECK(x86emu_history_enable(e) == 0);

  x86emu_history_stats(e, &st);
  CHECK(st.entries == 0 && st.data_bytes == 0 && st.attr_bytes == 0);

  /* peek does not materialize and reports 0xff + default attributes */
  CHECK(!page_materialized(e, 0x90000));
  x86emu_history_peek(e, 0x90000, 1, &d, &a);
  CHECK(d == 0xff);
  CHECK(a == X86EMU_PERM_RWX);
  CHECK(!page_materialized(e, 0x90000));

  /* peek does not set access bits */
  CHECK(peek_attr(e, 0x100) == (X86EMU_PERM_RWX | X86EMU_PERM_VALID | X86EMU_ACC_W));

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);

  x86emu_history_stats(e, &st);
  CHECK(st.entries == 1);
  CHECK(st.data_bytes == 2);        /* only the two written bytes */
  CHECK(st.attr_bytes < 64);        /* instruction fetch + written bytes */
  CHECK(st.data_bytes + st.attr_bytes < 4096);

  CHECK(peek8(e, 0x2000) == 0x34 && peek8(e, 0x2001) == 0x12);

  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  x86emu_history_stats(e, &st);
  CHECK(st.entries == 0 && st.data_bytes == 0 && st.attr_bytes == 0);
  CHECK(peek8(e, 0x2000) == 0);         /* pre-step content restored */

  x86emu_history_disable(e);
  x86emu_done(e);
}

/*
 * Public memory writes, mapping/permission changes, reset_access_stats,
 * reset and plain run all invalidate a recorded history.
 */
static void t_memory_invalidate_apis(void)
{
  static const u8 code[] = { 0xb8, 0x01, 0x00, 0xf4 };  /* mov ax,1 ; hlt */
  x86emu_t *e;
  unsigned u;

  /* public memory write */
  e = mk_emu();
  load(e, 0x100, code, sizeof code);
  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  x86emu_write_byte(e, 0x300, 1);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_DISABLED);
  x86emu_done(e);

  /* permission change */
  e = mk_emu();
  load(e, 0x100, code, sizeof code);
  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  x86emu_set_perm(e, 0x4000, 0x4fff, X86EMU_PERM_RX);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);
  x86emu_done(e);

  /* page mapping */
  e = mk_emu();
  load(e, 0x100, code, sizeof code);
  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  x86emu_set_page(e, 0x4000, NULL);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);
  x86emu_done(e);

  /* access statistics reset */
  e = mk_emu();
  load(e, 0x100, code, sizeof code);
  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  x86emu_reset_access_stats(e);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);
  x86emu_done(e);

  /* reset */
  e = mk_emu();
  load(e, 0x100, code, sizeof code);
  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  x86emu_reset(e);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);
  x86emu_done(e);

  /* plain run */
  e = mk_emu();
  load(e, 0x100, code, sizeof code);
  CHECK(x86emu_history_enable(e) == 0);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  e->max_instr = 10;
  u = x86emu_run(e, X86EMU_RUN_MAX_INSTR);
  CHECK(!(u & X86EMU_RUN_MAX_INSTR));
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);
  x86emu_done(e);
}

/* ------------------------------------------------------------------ */
/* group: barriers                                                     */
/* ------------------------------------------------------------------ */

/* Port i/o instructions are not executed; cpu and memory stay untouched. */
static void t_barriers_io(void)
{
  static const u8 code[] = {
    0xe6, 0x80,   /* 0x100: out 0x80,al */
    0xe4, 0x80,   /* 0x102: in al,0x80 */
    0xec,         /* 0x104: in al,dx */
    0xee,         /* 0x105: out dx,al */
    0xf4          /* 0x106: hlt */
  };
  x86emu_t *e = mk_emu();
  x86emu_history_stats_t st;

  load(e, 0x100, code, sizeof code);
  e->x86.R_AX = 0x55;

  CHECK(x86emu_history_enable(e) == 0);

  /* each step hits the barrier before anything happens */
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_BARRIER);
  CHECK(e->x86.R_EIP == 0x100);
  CHECK(e->x86.R_AX == 0x55);
  CHECK(e->mem->invalid == 0);

  x86emu_history_stats(e, &st);
  CHECK(st.entries == 0);

  /* skip to the hlt: registers may be changed at an instruction boundary */
  e->x86.R_EIP = 0x106;
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_HLT);

  x86emu_history_disable(e);
  x86emu_done(e);
}

static unsigned cpuid_calls, intr_calls;

static void count_cpuid(x86emu_t *emu)
{
  (void) emu;
  cpuid_calls++;
}

static int count_intr(x86emu_t *emu, u8 num, unsigned type)
{
  (void) emu; (void) num; (void) type;
  intr_calls++;
  return 0;
}

/*
 * CPUID, RDTSC, RDMSR/WRMSR and INT stop at a barrier before any handler
 * runs: counting (but otherwise inert) callbacks prove the host was never
 * called.
 */
static void t_barriers_callbacks(void)
{
  static const u8 code[] = {
    0x0f, 0xa2,   /* 0x100: cpuid */
    0x0f, 0x31,   /* 0x102: rdtsc */
    0x0f, 0x32,   /* 0x104: rdmsr */
    0x0f, 0x30,   /* 0x106: wrmsr */
    0xcd, 0x80,   /* 0x108: int 0x80 */
    0xf4          /* 0x10a: hlt */
  };
  x86emu_t *e = mk_emu();
  unsigned u;

  x86emu_set_cpuid_handler(e, count_cpuid);
  x86emu_set_intr_handler(e, count_intr);

  load(e, 0x100, code, sizeof code);

  cpuid_calls = intr_calls = 0;

  CHECK(x86emu_history_enable(e) == 0);

  for(u = 0; u < 5; u++) {
    CHECK(x86emu_history_step(e) == X86EMU_HISTORY_BARRIER);
    CHECK(e->x86.R_EIP == 0x100);
  }

  CHECK(cpuid_calls == 0);
  CHECK(intr_calls == 0);

  e->x86.R_EIP = 0x10a;
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_HLT);

  CHECK(cpuid_calls == 0);
  CHECK(intr_calls == 0);

  x86emu_history_disable(e);
  x86emu_done(e);
}

static unsigned dummy_memio(x86emu_t *emu, u32 addr, u32 *val, unsigned type)
{
  (void) emu; (void) addr; (void) val; (void) type;
  return 0;
}

static int dummy_code(x86emu_t *emu)
{
  (void) emu;
  return 0;
}

/* Enabling history with a custom memio or code handler is refused. */
static void t_barriers_enable_rejected(void)
{
  x86emu_t *e = mk_emu();
  x86emu_memio_handler_t old_memio;

  old_memio = x86emu_set_memio_handler(e, dummy_memio);
  CHECK(x86emu_history_enable(e) != 0);
  x86emu_set_memio_handler(e, old_memio);

  x86emu_set_code_handler(e, dummy_code);
  CHECK(x86emu_history_enable(e) != 0);
  x86emu_set_code_handler(e, NULL);

  CHECK(x86emu_history_enable(e) == 0);
  x86emu_history_disable(e);

  x86emu_done(e);
}

/* ------------------------------------------------------------------ */
/* group: branching                                                    */
/* ------------------------------------------------------------------ */

/*
 * Undoing steps and then changing a register starts a new branch: the
 * old future is gone and cannot be restored.
 */
static void t_branching_register_branch(void)
{
  static const u8 code[] = {
    0xb8, 0x01, 0x00,   /* 0x100: mov ax,1 */
    0xbb, 0x02, 0x00,   /* 0x103: mov bx,2 */
    0xb9, 0x03, 0x00,   /* 0x106: mov cx,3 */
    0xf4                /* 0x109: hlt */
  };
  x86emu_t *e = mk_emu();
  x86emu_history_stats_t st;

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(e->x86.R_AX == 1 && e->x86.R_BX == 2 && e->x86.R_CX == 3);

  /* go back before "mov bx,2" and start a new branch */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(e->x86.R_AX == 1 && e->x86.R_BX == 0 && e->x86.R_CX == 0);

  e->x86.R_BX = 0x77;   /* allowed: register change at instruction boundary */

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(e->x86.R_BX == 2);

  /* the new branch replaced the old future */
  x86emu_history_stats(e, &st);
  CHECK(st.entries == 2);

  /* undoing the branch step restores the boundary register value */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(e->x86.R_BX == 0x77);

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(e->x86.R_BX == 2);

  /* only the new branch exists: two backs, then empty */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  x86emu_history_disable(e);
  x86emu_done(e);
}

/* HLT can be undone; stepping a halted cpu is a no-op; empty is explicit. */
static void t_branching_hlt_back_and_empty(void)
{
  static const u8 code[] = {
    0xb8, 0x2a, 0x00,   /* 0x100: mov ax,0x2a */
    0xf4                /* 0x103: hlt */
  };
  x86emu_t *e = mk_emu();
  x86emu_history_stats_t st;

  load(e, 0x100, code, sizeof code);

  CHECK(x86emu_history_enable(e) == 0);

  /* nothing to undo yet */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_HLT);
  CHECK(e->x86.mode & 0x80);    /* _MODE_HALTED */

  /* stepping a halted cpu changes nothing */
  x86emu_history_stats(e, &st);
  CHECK(st.entries == 2);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_HLT);
  x86emu_history_stats(e, &st);
  CHECK(st.entries == 2);

  /* undo the hlt: the cpu is no longer halted */
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(!(e->x86.mode & 0x80));
  CHECK(e->x86.R_EIP == 0x103);

  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_OK);
  CHECK(e->x86.R_AX == 0);
  CHECK(x86emu_history_back(e) == X86EMU_HISTORY_BACK_EMPTY);

  /* re-execution reaches the same stop state */
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_OK);
  CHECK(x86emu_history_step(e) == X86EMU_HISTORY_HLT);
  CHECK(e->x86.R_AX == 0x2a);
  CHECK(e->x86.mode & 0x80);

  x86emu_history_disable(e);
  x86emu_done(e);
}

/* ------------------------------------------------------------------ */

void register_contract(void)
{
  contract_add("instructions", "alu_stack_call_ret_16", t_instructions_alu_stack_call_ret_16);
  contract_add("instructions", "string_ops_16", t_instructions_string_ops_16);
  contract_add("instructions", "mode32", t_instructions_mode32);
  contract_add("instructions", "disable_then_normal_run", t_instructions_disable_then_normal_run);

  contract_add("memory", "rep_cross_page_failure", t_memory_rep_cross_page_failure);
  contract_add("memory", "external_alias", t_memory_external_alias);
  contract_add("memory", "peek_and_stats", t_memory_peek_and_stats);
  contract_add("memory", "invalidate_apis", t_memory_invalidate_apis);

  contract_add("barriers", "io", t_barriers_io);
  contract_add("barriers", "callbacks", t_barriers_callbacks);
  contract_add("barriers", "enable_rejected", t_barriers_enable_rejected);

  contract_add("branching", "register_branch", t_branching_register_branch);
  contract_add("branching", "hlt_back_and_empty", t_branching_hlt_back_and_empty);
}
