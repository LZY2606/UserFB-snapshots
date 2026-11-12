/* checks/slice_cases.c - tests for the libudis86 backward slicing API.
 *
 * Each case uses hand-assembled bytes and asserts independently derived
 * instruction offsets, written-bit masks and entry (still live at the
 * fragment entry) masks.  On success every case prints "PASS <name>" and
 * main prints "SLICE_OK cases=7".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "udis86.h"
#include "libudis86/slice.h"

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      exit(1);                                                          \
    }                                                                   \
  } while (0)

static struct ud_slice *
run_slice(const uint8_t *code, size_t n, uint64_t reg0, uint8_t flags,
          int expect)
{
  uint64_t mask[UD_SLICE_NUM_REGS];
  struct ud_slice *s = NULL;
  memset(mask, 0, sizeof(mask));
  mask[UD_SLICE_RAX] = reg0;
  CHECK(ud_slice(code, n, mask, flags, &s) == expect);
  CHECK(s != NULL);
  CHECK(s->status == expect);
  return s;
}

/* mov ah, 0x7f ; mov al, cl -- overwriting AH must not hide the AL read */
static void
case_byte_alias(void)
{
  const uint8_t code[] = { 0xb4, 0x7f, 0x88, 0xc8 };
  struct ud_slice *s = run_slice(code, sizeof code, 0xff, 0, UD_SLICE_OK);

  CHECK(s->insn_count == 1);
  CHECK(s->insns[0].offset == 2);
  CHECK(s->insns[0].reg_bits[UD_SLICE_RAX] == 0xff);
  CHECK(s->insns[0].flag_bits == 0);
  CHECK(s->entry_regs[UD_SLICE_RAX] == 0);
  CHECK(s->entry_regs[UD_SLICE_RCX] == 0xff);
  CHECK(s->entry_flags == 0);
  ud_slice_free(s);
  puts("PASS byte_alias");
}

/* movzx edx, cl ; mov eax, edx -- 32 bit writes determine the top half */
static void
case_zero_extend(void)
{
  const uint8_t code[] = { 0x0f, 0xb6, 0xd1, 0x89, 0xd0 };
  struct ud_slice *s = run_slice(code, sizeof code, ~0ull, 0, UD_SLICE_OK);

  CHECK(s->insn_count == 2);
  CHECK(s->insns[0].offset == 0);
  CHECK(s->insns[0].reg_bits[UD_SLICE_RDX] == 0xffffffffull);
  CHECK(s->insns[1].offset == 3);
  CHECK(s->insns[1].reg_bits[UD_SLICE_RAX] == ~0ull);
  /* the high half of rax is a constant zero: nothing is live at entry */
  CHECK(s->entry_regs[UD_SLICE_RAX] == 0);
  CHECK(s->entry_regs[UD_SLICE_RDX] == 0);
  CHECK(s->entry_regs[UD_SLICE_RCX] == 0xff);
  CHECK(s->entry_flags == 0);
  ud_slice_free(s);

  /* querying only the high half still keeps the constant-writing mov */
  {
    struct ud_slice *t =
      run_slice(code + 3, 2, 0xffffffff00000000ull, 0, UD_SLICE_OK);
    CHECK(t->insn_count == 1);
    CHECK(t->insns[0].offset == 0);
    CHECK(t->insns[0].reg_bits[UD_SLICE_RAX] == 0xffffffff00000000ull);
    CHECK(t->entry_regs[UD_SLICE_RAX] == 0);
    CHECK(t->entry_regs[UD_SLICE_RCX] == 0);
    ud_slice_free(t);
  }
  puts("PASS zero_extend");
}

/* test eax,eax ; setns cl ; cmp ecx,edx ; setz al -- CMP and TEST feed
 * different flags to the conditional instructions that follow them */
static void
case_flag_chain(void)
{
  const uint8_t code[] = {
    0x85, 0xc0,             /* test eax, eax  */
    0x0f, 0x99, 0xc1,       /* setns cl       */
    0x39, 0xd1,             /* cmp ecx, edx   */
    0x0f, 0x94, 0xc0        /* setz al        */
  };
  uint64_t mask[UD_SLICE_NUM_REGS];
  struct ud_slice *s = NULL;

  memset(mask, 0, sizeof(mask));
  mask[UD_SLICE_RAX] = 0xff;
  mask[UD_SLICE_RCX] = 0xff;
  CHECK(ud_slice(code, sizeof code, mask, 0, &s) == UD_SLICE_OK);

  CHECK(s->insn_count == 4);
  CHECK(s->insns[0].offset == 0);
  CHECK(s->insns[0].flag_bits == UD_SLICE_SF);
  CHECK(s->insns[1].offset == 2);
  CHECK(s->insns[1].reg_bits[UD_SLICE_RCX] == 0xff);
  CHECK(s->insns[1].flag_bits == 0);
  CHECK(s->insns[2].offset == 5);
  CHECK(s->insns[2].flag_bits == UD_SLICE_ZF);
  CHECK(s->insns[3].offset == 7);
  CHECK(s->insns[3].reg_bits[UD_SLICE_RAX] == 0xff);
  /* SF of test eax,eax depends only on the sign bit of eax */
  CHECK(s->entry_regs[UD_SLICE_RAX] == 0x80000000ull);
  /* cmp flags conservatively depend on the whole operands */
  CHECK(s->entry_regs[UD_SLICE_RCX] == 0xffffff00ull);
  CHECK(s->entry_regs[UD_SLICE_RDX] == 0xffffffffull);
  CHECK(s->entry_flags == 0);
  ud_slice_free(s);

  /* and eax, 0xff leaves AF undefined: tracking AF must be reported */
  {
    const uint8_t logic[] = { 0x83, 0xe0, 0xff };
    struct ud_slice *t = NULL;
    memset(mask, 0, sizeof(mask));
    CHECK(ud_slice(logic, sizeof logic, mask, UD_SLICE_AF, &t) ==
          UD_SLICE_ERR_UNDEF_FLAG);
    CHECK(t != NULL);
    CHECK(t->error_offset == 0);
    CHECK(t->error_flag == UD_SLICE_AF);
    CHECK(t->insn_count == 0);
    CHECK(t->insns == NULL);
    ud_slice_free(t);
  }
  puts("PASS flag_chain");
}

/* test dl,dl ; cmovz eax,ecx ; setnz al -- CMOV keeps the old destination
 * as a possible source and depends on the flags its condition reads */
static void
case_conditional(void)
{
  const uint8_t code[] = {
    0x84, 0xd2,             /* test dl, dl    */
    0x0f, 0x44, 0xc1,       /* cmovz eax, ecx */
    0x0f, 0x95, 0xc0        /* setnz al       */
  };
  struct ud_slice *s = run_slice(code, sizeof code, 0xffffffffull, 0,
                                 UD_SLICE_OK);

  CHECK(s->insn_count == 3);
  CHECK(s->insns[0].offset == 0);
  CHECK(s->insns[0].flag_bits == UD_SLICE_ZF);
  CHECK(s->insns[1].offset == 2);
  CHECK(s->insns[1].reg_bits[UD_SLICE_RAX] == 0xffffff00ull);
  CHECK(s->insns[2].offset == 5);
  CHECK(s->insns[2].reg_bits[UD_SLICE_RAX] == 0xff);
  /* the old eax and ecx both feed the cmov result; dl feeds the flags */
  CHECK(s->entry_regs[UD_SLICE_RAX] == 0xffffff00ull);
  CHECK(s->entry_regs[UD_SLICE_RCX] == 0xffffff00ull);
  CHECK(s->entry_regs[UD_SLICE_RDX] == 0xff);
  CHECK(s->entry_flags == 0);
  ud_slice_free(s);
  puts("PASS conditional");
}

/* mov ah, cl -- an 8 bit write preserves the rest of the destination */
static void
case_old_destination(void)
{
  const uint8_t code[] = { 0x88, 0xcc };
  struct ud_slice *s = run_slice(code, sizeof code, ~0ull, 0, UD_SLICE_OK);

  CHECK(s->insn_count == 1);
  CHECK(s->insns[0].offset == 0);
  CHECK(s->insns[0].reg_bits[UD_SLICE_RAX] == 0xff00ull);
  CHECK(s->entry_regs[UD_SLICE_RAX] == 0xffffffffffff00ffull);
  CHECK(s->entry_regs[UD_SLICE_RCX] == 0xff);
  CHECK(s->entry_flags == 0);
  ud_slice_free(s);
  puts("PASS old_destination");
}

/* memory operands, branches and returns are rejected with their offset,
 * truncated instructions are decode errors, and no half result is made */
static void
case_unsupported(void)
{
  const uint8_t memop[] = { 0x89, 0xc8, 0x8b, 0x01, 0x89, 0xd0 };
  const uint8_t ret[]   = { 0x89, 0xc8, 0xc3 };
  const uint8_t trunc[] = { 0x89, 0xc8, 0x0f, 0x44 };
  struct ud_slice *s = NULL;

  CHECK(ud_slice(memop, sizeof memop, NULL, 0, &s) ==
        UD_SLICE_ERR_UNSUPPORTED);
  CHECK(s != NULL);
  CHECK(s->error_offset == 2);
  CHECK(s->insn_count == 0);
  CHECK(s->insns == NULL);
  ud_slice_free(s);

  s = NULL;
  CHECK(ud_slice(ret, sizeof ret, NULL, 0, &s) == UD_SLICE_ERR_UNSUPPORTED);
  CHECK(s->error_offset == 2);
  ud_slice_free(s);

  s = NULL;
  CHECK(ud_slice(trunc, sizeof trunc, NULL, 0, &s) == UD_SLICE_ERR_DECODE);
  CHECK(s->error_offset == 2);
  CHECK(s->insn_count == 0);
  CHECK(s->insns == NULL);
  ud_slice_free(s);
  puts("PASS unsupported");
}

/* repeated calls and freeing results must not disturb the input or each
 * other, and a failure must not poison later calls */
static void
case_ownership(void)
{
  const uint8_t code[] = { 0xb4, 0x7f, 0x88, 0xc8 };
  const uint8_t bad[]  = { 0xc3 };
  uint8_t expect[sizeof code];
  uint64_t mask[UD_SLICE_NUM_REGS];
  struct ud_slice *a = NULL, *b = NULL, *c = NULL;
  int g;

  memcpy(expect, code, sizeof code);
  memset(mask, 0, sizeof(mask));
  mask[UD_SLICE_RAX] = 0xff;

  CHECK(ud_slice(code, sizeof code, mask, 0, &a) == UD_SLICE_OK);
  CHECK(ud_slice(code, sizeof code, mask, 0, &b) == UD_SLICE_OK);
  /* identical inputs give identical results */
  CHECK(a->insn_count == b->insn_count);
  CHECK(a->entry_flags == b->entry_flags);
  CHECK(memcmp(a->entry_regs, b->entry_regs, sizeof a->entry_regs) == 0);
  for (g = 0; g < (int)a->insn_count; g++) {
    CHECK(a->insns[g].offset == b->insns[g].offset);
    CHECK(a->insns[g].flag_bits == b->insns[g].flag_bits);
    CHECK(memcmp(a->insns[g].reg_bits, b->insns[g].reg_bits,
                 sizeof a->insns[g].reg_bits) == 0);
  }
  /* freeing one result must not affect the other */
  ud_slice_free(a);
  CHECK(b->insn_count == 1);
  CHECK(b->insns[0].offset == 2);
  CHECK(b->entry_regs[UD_SLICE_RCX] == 0xff);

  /* a failing call in between must not poison later calls */
  CHECK(ud_slice(bad, sizeof bad, mask, 0, &c) == UD_SLICE_ERR_UNSUPPORTED);
  ud_slice_free(c);
  c = NULL;
  CHECK(ud_slice(code, sizeof code, mask, 0, &c) == UD_SLICE_OK);
  CHECK(c->insn_count == b->insn_count);
  CHECK(c->insns[0].offset == b->insns[0].offset);
  CHECK(c->entry_regs[UD_SLICE_RCX] == b->entry_regs[UD_SLICE_RCX]);

  ud_slice_free(b);
  ud_slice_free(c);
  ud_slice_free(NULL);

  /* the input buffer is read-only for the library */
  CHECK(memcmp(code, expect, sizeof code) == 0);
  puts("PASS ownership");
}

int
main(void)
{
  case_byte_alias();
  case_zero_extend();
  case_flag_chain();
  case_conditional();
  case_old_destination();
  case_unsupported();
  case_ownership();
  puts("SLICE_OK cases=7");
  return 0;
}
