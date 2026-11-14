/* tests/frame_cases.c - frame analysis task cases.
 *
 * Registered with the task driver via task_cases/task_case_count; no main
 * here.  All byte arrays are hand-assembled x86-64 and all expected
 * offsets are hand computed relative to the entry RSP value S.
 */
#include "task_support/check.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libudis86/frame.h"

#define OFF UD_FRAME_VALUE_OFFSET
#define UNK UD_FRAME_VALUE_UNKNOWN

static int
v_eq(struct ud_frame_value v, int kind, int64_t off)
{
  return (int) v.kind == kind &&
         (kind != OFF || v.offset == off);
}

static int
a_eq(const struct ud_frame_access *a, int kind, unsigned operand,
     unsigned size, int vkind, int64_t off)
{
  return (int) a->kind == kind && a->operand == operand &&
         a->size == size && v_eq(a->address, vkind, off);
}

static const struct ud_frame_block *
find_block(const struct ud_frame_report *r, uint64_t begin)
{
  size_t i;
  for (i = 0; i < r->block_count; ++i)
    if (r->blocks[i].begin == begin)
      return &r->blocks[i];
  return NULL;
}

static enum ud_frame_status
analyze(const uint8_t *code, size_t code_size, size_t entry,
        const struct ud_frame_block_desc *blocks, size_t block_count,
        struct ud_frame_report **report, struct ud_frame_error *error)
{
  struct ud_frame_graph g;
  g.code = code;
  g.code_size = code_size;
  g.entry = entry;
  g.blocks = blocks;
  g.block_count = block_count;
  return ud_frame_analyze(&g, report, error);
}

/* ------------------------------------------------------------------ */
/* normalize: malformed graphs are rejected with block/offset detail  */
/* ------------------------------------------------------------------ */

static int
t_invalid_graphs(void)
{
  static const uint8_t code[] = { 0x90, 0x90, 0x90 };
  static const struct ud_frame_block_desc empty_range[] = {
    { 0, 0, NULL, 0 },
  };
  static const struct ud_frame_block_desc past_end[] = {
    { 0, 99, NULL, 0 },
  };
  static const size_t ov_succ0[] = { 1 };
  static const struct ud_frame_block_desc overlap[] = {
    { 0, 2, ov_succ0, 1 },
    { 1, 3, NULL, 0 },
  };
  static const size_t bad_succ[] = { 7 };
  static const struct ud_frame_block_desc bad_edge[] = {
    { 0, 1, bad_succ, 1 },
  };
  static const size_t self_succ[] = { 0 };
  static const struct ud_frame_block_desc one[] = {
    { 0, 1, self_succ, 1 },
  };
  struct ud_frame_report *rep = (void *) 0;
  struct ud_frame_error err;

  memset(&err, 0, sizeof(err));

  /* NULL graph / NULL report pointer */
  TASK_CHECK(ud_frame_analyze(NULL, &rep, &err) == UD_FRAME_E_ARGUMENT);
  TASK_CHECK(rep == NULL);
  TASK_CHECK(err.code == UD_FRAME_E_ARGUMENT);
  TASK_CHECK(err.block == UD_FRAME_NO_BLOCK);

  /* entry index out of range */
  rep = (void *) 0;
  TASK_CHECK(analyze(code, sizeof(code), 5, one, 1, &rep, &err)
             == UD_FRAME_E_ARGUMENT);
  TASK_CHECK(rep == NULL);

  /* empty range */
  rep = (void *) 0;
  TASK_CHECK(analyze(code, sizeof(code), 0, empty_range, 1, &rep, &err)
             == UD_FRAME_E_RANGE);
  TASK_CHECK(err.block == 0 && err.offset == 0);
  TASK_CHECK(rep == NULL);

  /* range past the end of the code */
  rep = (void *) 0;
  TASK_CHECK(analyze(code, sizeof(code), 0, past_end, 1, &rep, &err)
             == UD_FRAME_E_RANGE);
  TASK_CHECK(err.block == 0);
  TASK_CHECK(rep == NULL);

  /* overlapping ranges */
  rep = (void *) 0;
  TASK_CHECK(analyze(code, sizeof(code), 0, overlap, 2, &rep, &err)
             == UD_FRAME_E_RANGE);
  TASK_CHECK(err.block == 1 && err.offset == 1);
  TASK_CHECK(rep == NULL);

  /* successor index out of range */
  rep = (void *) 0;
  TASK_CHECK(analyze(code, sizeof(code), 0, bad_edge, 1, &rep, &err)
             == UD_FRAME_E_EDGE);
  TASK_CHECK(err.block == 0 && err.offset == 0);
  TASK_CHECK(rep == NULL);

  /* a well-formed single-block graph passes validation */
  rep = NULL;
  TASK_CHECK(analyze(code, sizeof(code), 0, one, 1, &rep, &err)
             == UD_FRAME_OK);
  TASK_CHECK(rep != NULL);
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* normalize: decode/unsupported failures, unreachable blocks         */
/* ------------------------------------------------------------------ */

static int
t_decode_errors(void)
{
  static const uint8_t trunc[] = { 0x48, 0x83 };          /* cut mid-insn */
  static const uint8_t illegal[] = { 0xff, 0xff };        /* invalid /7   */
  static const uint8_t unsup[] = { 0x0f, 0x05 };          /* syscall      */
  static const uint8_t unsup_at1[] = { 0x90, 0x0f, 0x05 };
  static const uint8_t dead_unsup[] = { 0xc3, 0x0f, 0x05 };
  static const struct ud_frame_block_desc b_trunc[] = { { 0, 2, NULL, 0 } };
  static const struct ud_frame_block_desc b_unsup3[] = { { 0, 3, NULL, 0 } };
  static const struct ud_frame_block_desc b_dead[] = {
    { 0, 1, NULL, 0 },
    { 1, 3, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;

  TASK_CHECK(analyze(trunc, sizeof(trunc), 0, b_trunc, 1, &rep, &err)
             == UD_FRAME_E_DECODE);
  TASK_CHECK(err.block == 0 && err.offset == 0);
  TASK_CHECK(rep == NULL);

  TASK_CHECK(analyze(illegal, sizeof(illegal), 0, b_trunc, 1, &rep, &err)
             == UD_FRAME_E_DECODE);
  TASK_CHECK(err.block == 0 && err.offset == 0);
  TASK_CHECK(rep == NULL);

  TASK_CHECK(analyze(unsup, sizeof(unsup), 0, b_trunc, 1, &rep, &err)
             == UD_FRAME_E_UNSUPPORTED);
  TASK_CHECK(err.block == 0 && err.offset == 0);
  TASK_CHECK(rep == NULL);

  /* unsupported instruction after a supported one keeps its offset */
  TASK_CHECK(analyze(unsup_at1, sizeof(unsup_at1), 0, b_unsup3, 1, &rep, &err)
             == UD_FRAME_E_UNSUPPORTED);
  TASK_CHECK(err.block == 0 && err.offset == 1);
  TASK_CHECK(rep == NULL);

  /* the same bytes in an unreachable block are never decoded */
  rep = NULL;
  TASK_CHECK(analyze(dead_unsup, sizeof(dead_unsup), 0, b_dead, 2, &rep, &err)
             == UD_FRAME_OK);
  TASK_CHECK(rep != NULL);
  TASK_CHECK(rep->block_count == 1);
  TASK_CHECK(rep->blocks[0].index == 0);
  TASK_CHECK(rep->blocks[0].insn_count == 1);
  ud_frame_report_free(rep);
  return 0;
}

static int
t_unreachable(void)
{
  static const uint8_t code[] = { 0xc3, 0x90, 0x90, 0xc3 };
  static const size_t s0[] = { 2 };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 1, s0, 1 },      /* 0: ret, edges to 2        */
    { 1, 3, NULL, 0 },    /* 1: unreachable nops       */
    { 3, 4, NULL, 0 },    /* 2: ret                    */
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 3, &rep, &err)
             == UD_FRAME_OK);
  TASK_CHECK(rep != NULL);
  /* no instruction results for the unreachable block, and the reachable
     blocks come out sorted by start offset */
  TASK_CHECK(rep->block_count == 2);
  TASK_CHECK(rep->blocks[0].begin == 0 && rep->blocks[0].index == 0);
  TASK_CHECK(rep->blocks[1].begin == 3 && rep->blocks[1].index == 2);
  TASK_CHECK(rep->blocks[0].insn_count == 1);
  TASK_CHECK(rep->blocks[0].insns[0].offset == 0);
  TASK_CHECK(v_eq(rep->blocks[0].insns[0].rsp, OFF, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* normalize: reuse after failure and report ownership                */
/* ------------------------------------------------------------------ */

static int
t_reuse_ownership(void)
{
  /* push rbp; mov rbp,rsp; ret */
  uint8_t code[] = { 0x55, 0x48, 0x89, 0xe5, 0xc3 };
  struct ud_frame_block_desc descs[1];
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  descs[0].begin = 0;
  descs[0].end = 99;              /* bad range: fails first */
  descs[0].successors = NULL;
  descs[0].successor_count = 0;

  TASK_CHECK(analyze(code, sizeof(code), 0, descs, 1, &rep, &err)
             == UD_FRAME_E_RANGE);
  TASK_CHECK(rep == NULL);

  /* the same output variables are reused after the failure */
  descs[0].end = 5;
  TASK_CHECK(analyze(code, sizeof(code), 0, descs, 1, &rep, &err)
             == UD_FRAME_OK);
  TASK_CHECK(rep != NULL);
  TASK_CHECK(rep->block_count == 1);
  b = &rep->blocks[0];
  TASK_CHECK(b->insn_count == 3);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, 0));
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[2].rbp, OFF, -8));

  /* the report is independently owned: scribbling over the input code
     and the graph description must not change it */
  memset(code, 0x90, sizeof(code));
  memset(descs, 0, sizeof(descs));
  b = &rep->blocks[0];
  TASK_CHECK(b->begin == 0 && b->end == 5);
  TASK_CHECK(b->insn_count == 3);
  TASK_CHECK(b->insns[0].offset == 0 && b->insns[0].size == 1);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, 0));
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[2].rbp, OFF, -8));
  TASK_CHECK(b->insns[0].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[0].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));
  ud_frame_report_free(rep);

  /* and the same pointer can be reused for a fresh analysis */
  rep = NULL;
  TASK_CHECK(analyze(code, sizeof(code), 0, descs, 1, &rep, &err)
             == UD_FRAME_E_RANGE); /* descs were zeroed: empty range */
  TASK_CHECK(rep == NULL);
  return 0;
}

/* ------------------------------------------------------------------ */
/* normalize: renumbering blocks and edges keeps the meaning          */
/* ------------------------------------------------------------------ */

static int
reports_equivalent(const struct ud_frame_report *a,
                   const struct ud_frame_report *b)
{
  size_t i, j, k;
  if (a->block_count != b->block_count)
    return 0;
  for (i = 0; i < a->block_count; ++i) {
    const struct ud_frame_block *ba = &a->blocks[i];
    const struct ud_frame_block *bb = &b->blocks[i];
    if (ba->begin != bb->begin || ba->end != bb->end ||
        ba->insn_count != bb->insn_count)
      return 0;
    for (j = 0; j < ba->insn_count; ++j) {
      const struct ud_frame_insn *ia = &ba->insns[j];
      const struct ud_frame_insn *ib = &bb->insns[j];
      if (ia->offset != ib->offset || ia->size != ib->size ||
          !v_eq(ia->rsp, ib->rsp.kind, ib->rsp.offset) ||
          !v_eq(ia->rbp, ib->rbp.kind, ib->rbp.offset) ||
          ia->access_count != ib->access_count)
        return 0;
      for (k = 0; k < ia->access_count; ++k)
        if (!a_eq(&ia->accesses[k], ib->accesses[k].kind,
                  ib->accesses[k].operand, ib->accesses[k].size,
                  ib->accesses[k].address.kind,
                  ib->accesses[k].address.offset))
          return 0;
    }
  }
  return 1;
}

static int
t_renumber_stability(void)
{
  /* sub rsp,0x20; jz +0; sub rsp,0x10; ret */
  static const uint8_t code[] = {
    0x48, 0x83, 0xec, 0x20, 0x74, 0x00,
    0x48, 0x83, 0xec, 0x10,
    0xc3
  };
  static const size_t a_s0[] = { 1, 2 };
  static const size_t a_s1[] = { 2 };
  static const struct ud_frame_block_desc ga[] = {
    { 0, 6, a_s0, 2 },
    { 6, 10, a_s1, 1 },
    { 10, 11, NULL, 0 },
  };
  /* same graph, blocks and edge order shuffled */
  static const size_t b_s1[] = { 2, 0 };   /* edges of old block 0 */
  static const size_t b_s2[] = { 0 };      /* edges of old block 1 */
  static const struct ud_frame_block_desc gb[] = {
    { 10, 11, NULL, 0 },
    { 0, 6, b_s1, 2 },
    { 6, 10, b_s2, 1 },
  };
  struct ud_frame_report *ra = NULL, *rb = NULL;
  struct ud_frame_error err;

  TASK_CHECK(analyze(code, sizeof(code), 0, ga, 3, &ra, &err) == UD_FRAME_OK);
  TASK_CHECK(analyze(code, sizeof(code), 1, gb, 3, &rb, &err) == UD_FRAME_OK);
  TASK_CHECK(ra != NULL && rb != NULL);
  TASK_CHECK(ra->block_count == 3 && rb->block_count == 3);
  /* both reports are sorted by block start offset */
  TASK_CHECK(ra->blocks[0].begin == 0 && ra->blocks[1].begin == 6 &&
             ra->blocks[2].begin == 10);
  TASK_CHECK(rb->blocks[0].begin == 0 && rb->blocks[1].begin == 6 &&
             rb->blocks[2].begin == 10);
  /* the join sees S-32 from one side and S-48 from the other */
  TASK_CHECK(v_eq(ra->blocks[2].insns[0].rsp, UNK, 0));
  /* same meaning under renumbering (indices themselves may differ) */
  TASK_CHECK(ra->blocks[0].index != rb->blocks[0].index);
  TASK_CHECK(reports_equivalent(ra, rb));
  ud_frame_report_free(ra);
  ud_frame_report_free(rb);
  return 0;
}

/* ------------------------------------------------------------------ */
/* merge: diamonds                                                    */
/* ------------------------------------------------------------------ */

static int
t_diamond(void)
{
  /* sub rsp,0x20; jz +0  /  sub rsp,0x10  /  sub rsp,0x10  /  ret */
  static const uint8_t agree[] = {
    0x48, 0x83, 0xec, 0x20, 0x74, 0x00,
    0x48, 0x83, 0xec, 0x10,
    0x48, 0x83, 0xec, 0x10,
    0xc3
  };
  /* same shape, but one side leaves RSP alone */
  static const uint8_t disagree[] = {
    0x48, 0x83, 0xec, 0x20, 0x74, 0x00,
    0x48, 0x83, 0xec, 0x10,
    0x90, 0x90, 0x90, 0x90,
    0xc3
  };
  static const size_t s0[] = { 1, 2 };
  static const size_t s1[] = { 3 };
  static const size_t s2[] = { 3 };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 6, s0, 2 },
    { 6, 10, s1, 1 },
    { 10, 14, s2, 1 },
    { 14, 15, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(agree, sizeof(agree), 0, blocks, 4, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 2);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, 0));
  TASK_CHECK(v_eq(b->insns[1].rsp, OFF, -32));
  b = find_block(rep, 6);
  TASK_CHECK(b != NULL && v_eq(b->insns[0].rsp, OFF, -32));
  /* both sides subtract 0x10 more: the constant survives the join */
  b = find_block(rep, 14);
  TASK_CHECK(b != NULL && b->insn_count == 1);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, -48));
  TASK_CHECK(v_eq(b->insns[0].rbp, UNK, 0));
  TASK_CHECK(b->insns[0].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[0].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -48));
  ud_frame_report_free(rep);

  rep = NULL;
  TASK_CHECK(analyze(disagree, sizeof(disagree), 0, blocks, 4, &rep, &err)
             == UD_FRAME_OK);
  /* S-48 vs S-32: the join is unknown */
  b = find_block(rep, 14);
  TASK_CHECK(b != NULL);
  TASK_CHECK(v_eq(b->insns[0].rsp, UNK, 0));
  TASK_CHECK(v_eq(b->insns[0].rbp, UNK, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* merge: back edge repropagates until stable                         */
/* ------------------------------------------------------------------ */

static int
t_back_edge(void)
{
  /* sub rsp,0x20  /  loop: sub rsp,8  /  ret */
  static const uint8_t code[] = {
    0x48, 0x83, 0xec, 0x20,
    0x48, 0x83, 0xec, 0x08,
    0xc3
  };
  static const size_t s0[] = { 1 };
  static const size_t s1[] = { 1, 2 };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 4, s0, 1 },
    { 4, 8, s1, 2 },
    { 8, 9, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 3, &rep, &err)
             == UD_FRAME_OK);
  /* the loop head merges S-32 (entry) with S-40, S-48, ... (back edge):
     it must end up unknown, not keep the first value found */
  b = find_block(rep, 4);
  TASK_CHECK(b != NULL && b->insn_count == 1);
  TASK_CHECK(v_eq(b->insns[0].rsp, UNK, 0));
  b = find_block(rep, 8);
  TASK_CHECK(b != NULL);
  TASK_CHECK(v_eq(b->insns[0].rsp, UNK, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* merge: RBP stays known while RSP is unknown, and mov rsp,rbp       */
/* recovers RSP afterwards                                            */
/* ------------------------------------------------------------------ */

static int
t_rbp_recovery(void)
{
  /* push rbp / mov rbp,rsp / loop: sub rsp,8 / mov rsp,rbp / ret */
  static const uint8_t code[] = {
    0x55,
    0x48, 0x89, 0xe5,
    0x48, 0x83, 0xec, 0x08,
    0x48, 0x89, 0xec,
    0xc3
  };
  static const size_t s0[] = { 1 };
  static const size_t s1[] = { 2 };
  static const size_t s2[] = { 2, 3 };
  static const size_t s3[] = { 4 };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 1, s0, 1 },
    { 1, 4, s1, 1 },
    { 4, 8, s2, 2 },
    { 8, 11, s3, 1 },
    { 11, 12, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 5, &rep, &err)
             == UD_FRAME_OK);
  /* the frame base is loop-invariant: RSP is unknown at the loop head
     but RBP keeps its constant */
  b = find_block(rep, 4);
  TASK_CHECK(b != NULL);
  TASK_CHECK(v_eq(b->insns[0].rsp, UNK, 0));
  TASK_CHECK(v_eq(b->insns[0].rbp, OFF, -8));
  /* mov rsp,rbp restores RSP from the known RBP */
  b = find_block(rep, 8);
  TASK_CHECK(b != NULL);
  TASK_CHECK(v_eq(b->insns[0].rsp, UNK, 0));
  TASK_CHECK(v_eq(b->insns[0].rbp, OFF, -8));
  b = find_block(rep, 11);
  TASK_CHECK(b != NULL);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[0].rbp, OFF, -8));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: prologue, negative displacements, leave/ret slots         */
/* ------------------------------------------------------------------ */

static int
t_prologue_slots(void)
{
  /* push rbp; mov rbp,rsp; sub rsp,0x20; mov [rbp-8],rax;
     mov rax,[rbp-0x10]; leave; ret */
  static const uint8_t code[] = {
    0x55,
    0x48, 0x89, 0xe5,
    0x48, 0x83, 0xec, 0x20,
    0x48, 0x89, 0x45, 0xf8,
    0x48, 0x8b, 0x45, 0xf0,
    0xc9,
    0xc3
  };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 18, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 7);

  /* push rbp: implicit write of the slot below the old RSP */
  TASK_CHECK(b->insns[0].offset == 0 && b->insns[0].size == 1);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, 0));
  TASK_CHECK(v_eq(b->insns[0].rbp, UNK, 0));
  TASK_CHECK(b->insns[0].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[0].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));

  /* mov rbp,rsp */
  TASK_CHECK(b->insns[1].offset == 1);
  TASK_CHECK(v_eq(b->insns[1].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[1].rbp, UNK, 0));
  TASK_CHECK(b->insns[1].access_count == 0);

  /* sub rsp,0x20 */
  TASK_CHECK(b->insns[2].offset == 4);
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[2].rbp, OFF, -8));

  /* mov [rbp-8],rax: negative displacement relative to S */
  TASK_CHECK(b->insns[3].offset == 8);
  TASK_CHECK(v_eq(b->insns[3].rsp, OFF, -40));
  TASK_CHECK(v_eq(b->insns[3].rbp, OFF, -8));
  TASK_CHECK(b->insns[3].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[3].accesses[0], UD_FRAME_ACCESS_WRITE,
                  0, 64, OFF, -16));

  /* mov rax,[rbp-0x10]: a read */
  TASK_CHECK(b->insns[4].offset == 12);
  TASK_CHECK(b->insns[4].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[4].accesses[0], UD_FRAME_ACCESS_READ,
                  1, 64, OFF, -24));

  /* leave: reads the slot at the old RBP */
  TASK_CHECK(b->insns[5].offset == 16);
  TASK_CHECK(v_eq(b->insns[5].rsp, OFF, -40));
  TASK_CHECK(v_eq(b->insns[5].rbp, OFF, -8));
  TASK_CHECK(b->insns[5].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[5].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));

  /* ret: RSP is the restored S-8, RBP was popped to unknown */
  TASK_CHECK(b->insns[6].offset == 17);
  TASK_CHECK(v_eq(b->insns[6].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[6].rbp, UNK, 0));
  TASK_CHECK(b->insns[6].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[6].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: push/pop/call implicit slots and net-zero CALL            */
/* ------------------------------------------------------------------ */

static int
t_push_pop_call(void)
{
  /* push rax; push 0x10; pop rax; pop rbp; call +0; call rax; ret */
  static const uint8_t code[] = {
    0x50,
    0x6a, 0x10,
    0x58,
    0x5d,
    0xe8, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xd0,
    0xc3
  };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 13, NULL, 0 },
  };
  /* push rbp; mov rbp,rsp; call +0; ret */
  static const uint8_t code2[] = {
    0x55,
    0x48, 0x89, 0xe5,
    0xe8, 0x00, 0x00, 0x00, 0x00,
    0xc3
  };
  static const struct ud_frame_block_desc blocks2[] = {
    { 0, 10, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 7);

  TASK_CHECK(b->insns[0].offset == 0);
  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, 0));
  TASK_CHECK(a_eq(&b->insns[0].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));

  /* immediate push */
  TASK_CHECK(b->insns[1].offset == 1);
  TASK_CHECK(v_eq(b->insns[1].rsp, OFF, -8));
  TASK_CHECK(b->insns[1].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[1].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -16));

  /* pop rax reads the old RSP */
  TASK_CHECK(b->insns[2].offset == 3);
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, -16));
  TASK_CHECK(a_eq(&b->insns[2].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -16));

  /* pop rbp */
  TASK_CHECK(b->insns[3].offset == 4);
  TASK_CHECK(v_eq(b->insns[3].rsp, OFF, -8));
  TASK_CHECK(a_eq(&b->insns[3].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));

  /* direct call: writes the return slot, net stack change zero */
  TASK_CHECK(b->insns[4].offset == 5);
  TASK_CHECK(v_eq(b->insns[4].rsp, OFF, 0));
  TASK_CHECK(v_eq(b->insns[4].rbp, UNK, 0));
  TASK_CHECK(a_eq(&b->insns[4].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));

  /* indirect call: same convention */
  TASK_CHECK(b->insns[5].offset == 10);
  TASK_CHECK(v_eq(b->insns[5].rsp, OFF, 0));
  TASK_CHECK(a_eq(&b->insns[5].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, -8));

  /* ret reads the old RSP */
  TASK_CHECK(b->insns[6].offset == 12);
  TASK_CHECK(v_eq(b->insns[6].rsp, OFF, 0));
  TASK_CHECK(a_eq(&b->insns[6].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, 0));
  ud_frame_report_free(rep);

  /* CALL preserves a known RBP across the call */
  rep = NULL;
  TASK_CHECK(analyze(code2, sizeof(code2), 0, blocks2, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 4);
  TASK_CHECK(b->insns[3].offset == 9);
  TASK_CHECK(v_eq(b->insns[3].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[3].rbp, OFF, -8));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: POP RSP is a load, not just an add                        */
/* ------------------------------------------------------------------ */

static int
t_pop_rsp(void)
{
  /* pop rsp; push rax; ret */
  static const uint8_t code[] = { 0x5c, 0x50, 0xc3 };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 3, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 3);

  TASK_CHECK(v_eq(b->insns[0].rsp, OFF, 0));
  TASK_CHECK(a_eq(&b->insns[0].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, OFF, 0));

  /* the popped value comes from memory: RSP becomes unknown, and the
     following push still records its (unknown) slot */
  TASK_CHECK(v_eq(b->insns[1].rsp, UNK, 0));
  TASK_CHECK(b->insns[1].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[1].accesses[0], UD_FRAME_ACCESS_WRITE,
                  UD_FRAME_ACCESS_IMPLICIT, 64, UNK, 0));

  TASK_CHECK(v_eq(b->insns[2].rsp, UNK, 0));
  TASK_CHECK(b->insns[2].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[2].accesses[0], UD_FRAME_ACCESS_READ,
                  UD_FRAME_ACCESS_IMPLICIT, 64, UNK, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: LEA relations, alias destruction, AND                     */
/* ------------------------------------------------------------------ */

static int
t_alias_and_lea(void)
{
  static const uint8_t code[] = {
    0x48, 0x83, 0xec, 0x20,       /* 0:  sub rsp,0x20        */
    0x48, 0x8d, 0x64, 0x24, 0xf0, /* 4:  lea rsp,[rsp-0x10]  */
    0x48, 0x8d, 0x45, 0xd8,       /* 9:  lea rax,[rbp-0x28]  */
    0x48, 0x8d, 0x24, 0x0c,       /* 13: lea rsp,[rsp+rcx]   */
    0x89, 0xec,                   /* 17: mov esp,ebp         */
    0x66, 0x89, 0xe5,             /* 19: mov bp,sp           */
    0x48, 0x83, 0xe4, 0xf0,       /* 22: and rsp,-16         */
    0x48, 0x83, 0xc4, 0x08,       /* 26: add rsp,8           */
    0xc3                          /* 30: ret                 */
  };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 31, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 9);

  /* lea rsp,[rsp-0x10]: address arithmetic, not a memory read */
  TASK_CHECK(b->insns[1].offset == 4);
  TASK_CHECK(v_eq(b->insns[1].rsp, OFF, -32));
  TASK_CHECK(b->insns[1].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[1].accesses[0], UD_FRAME_ACCESS_ADDR,
                  1, 0, OFF, -48));
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, -48));

  /* lea rax,[rbp-0x28]: RBP unknown, so the address is unknown */
  TASK_CHECK(b->insns[2].offset == 9);
  TASK_CHECK(a_eq(&b->insns[2].accesses[0], UD_FRAME_ACCESS_ADDR,
                  1, 0, UNK, 0));

  /* lea rsp,[rsp+rcx]: an index register breaks the relation */
  TASK_CHECK(b->insns[3].offset == 13);
  TASK_CHECK(v_eq(b->insns[3].rsp, OFF, -48));
  TASK_CHECK(a_eq(&b->insns[3].accesses[0], UD_FRAME_ACCESS_ADDR,
                  1, 0, UNK, 0));
  TASK_CHECK(v_eq(b->insns[4].rsp, UNK, 0));

  /* mov esp,ebp: a 32-bit alias write is not "the low half of S" */
  TASK_CHECK(b->insns[4].offset == 17);
  TASK_CHECK(v_eq(b->insns[5].rsp, UNK, 0));

  /* mov bp,sp: 16-bit alias write destroys RBP too */
  TASK_CHECK(b->insns[5].offset == 19);
  TASK_CHECK(v_eq(b->insns[6].rbp, UNK, 0));

  /* and rsp,-16: always unknown */
  TASK_CHECK(b->insns[6].offset == 22);
  TASK_CHECK(v_eq(b->insns[7].rsp, UNK, 0));

  /* add on an unknown RSP stays unknown */
  TASK_CHECK(b->insns[7].offset == 26);
  TASK_CHECK(v_eq(b->insns[8].rsp, UNK, 0));
  TASK_CHECK(v_eq(b->insns[8].rbp, UNK, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: MOV register and memory forms, widths                     */
/* ------------------------------------------------------------------ */

static int
t_mov_forms(void)
{
  static const uint8_t code[] = {
    0x55,                   /* 0:  push rbp            */
    0x48, 0x89, 0xe5,       /* 1:  mov rbp,rsp         */
    0x48, 0x89, 0x65, 0xf0, /* 4:  mov [rbp-0x10],rsp  */
    0x48, 0x8b, 0x6d, 0xe8, /* 8:  mov rbp,[rbp-0x18]  */
    0x89, 0x45, 0xfc,       /* 12: mov [rbp-4],eax     */
    0x48, 0x89, 0x2c, 0x24, /* 15: mov [rsp],rbp       */
    0x8b, 0x04, 0x24,       /* 19: mov eax,[rsp]       */
    0xc3                    /* 22: ret                 */
  };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 23, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 8);

  /* mov [rbp-0x10],rsp: 64-bit write at S-24 */
  TASK_CHECK(b->insns[2].offset == 4);
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, -8));
  TASK_CHECK(v_eq(b->insns[2].rbp, OFF, -8));
  TASK_CHECK(b->insns[2].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[2].accesses[0], UD_FRAME_ACCESS_WRITE,
                  0, 64, OFF, -24));

  /* mov rbp,[rbp-0x18]: 64-bit read at S-32, RBP becomes unknown */
  TASK_CHECK(b->insns[3].offset == 8);
  TASK_CHECK(a_eq(&b->insns[3].accesses[0], UD_FRAME_ACCESS_READ,
                  1, 64, OFF, -32));
  TASK_CHECK(v_eq(b->insns[4].rbp, UNK, 0));

  /* mov [rbp-4],eax: 32-bit write, address unknown but still recorded */
  TASK_CHECK(b->insns[4].offset == 12);
  TASK_CHECK(b->insns[4].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[4].accesses[0], UD_FRAME_ACCESS_WRITE,
                  0, 32, UNK, 0));

  /* mov [rsp],rbp: 64-bit write at S-8 */
  TASK_CHECK(b->insns[5].offset == 15);
  TASK_CHECK(a_eq(&b->insns[5].accesses[0], UD_FRAME_ACCESS_WRITE,
                  0, 64, OFF, -8));

  /* mov eax,[rsp]: 32-bit read at S-8 */
  TASK_CHECK(b->insns[6].offset == 19);
  TASK_CHECK(a_eq(&b->insns[6].accesses[0], UD_FRAME_ACCESS_READ,
                  1, 32, OFF, -8));

  TASK_CHECK(b->insns[7].offset == 22);
  TASK_CHECK(v_eq(b->insns[7].rsp, OFF, -8));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: signed immediates on RSP                                  */
/* ------------------------------------------------------------------ */

static int
t_signed_imm(void)
{
  /* sub rsp,-16; add rsp,-16; ret */
  static const uint8_t code[] = {
    0x48, 0x83, 0xec, 0xf0,
    0x48, 0x83, 0xc4, 0xf0,
    0xc3
  };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 9, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 3);
  /* a negative immediate to SUB moves RSP up */
  TASK_CHECK(v_eq(b->insns[1].rsp, OFF, 16));
  /* and a negative immediate to ADD moves it back down */
  TASK_CHECK(v_eq(b->insns[2].rsp, OFF, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */
/* effects: FS/GS and address-size overrides, low-byte aliases        */
/* ------------------------------------------------------------------ */

static int
t_segment_and_alias(void)
{
  static const uint8_t code[] = {
    0x55,                         /* 0:  push rbp            */
    0x48, 0x89, 0xe5,             /* 1:  mov rbp,rsp         */
    0x64, 0x48, 0x8b, 0x45, 0xf8, /* 4:  mov rax,fs:[rbp-8]  */
    0x67, 0x48, 0x8b, 0x45, 0xf0, /* 9:  mov rax,[ebp-16]    */
    0x48, 0x8b, 0x45, 0xe8,       /* 14: mov rax,[rbp-0x18]  */
    0xc3                          /* 18: ret                 */
  };
  static const struct ud_frame_block_desc blocks[] = {
    { 0, 19, NULL, 0 },
  };
  /* sub rsp,0x20; mov spl,bpl; ret */
  static const uint8_t code2[] = {
    0x48, 0x83, 0xec, 0x20,
    0x40, 0x88, 0xec,
    0xc3
  };
  static const struct ud_frame_block_desc blocks2[] = {
    { 0, 8, NULL, 0 },
  };
  struct ud_frame_report *rep = NULL;
  struct ud_frame_error err;
  const struct ud_frame_block *b;

  TASK_CHECK(analyze(code, sizeof(code), 0, blocks, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 6);

  /* FS override: the address is unknown but the read is still recorded */
  TASK_CHECK(b->insns[2].offset == 4);
  TASK_CHECK(b->insns[2].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[2].accesses[0], UD_FRAME_ACCESS_READ,
                  1, 64, UNK, 0));

  /* address-size override: also unknown */
  TASK_CHECK(b->insns[3].offset == 9);
  TASK_CHECK(b->insns[3].access_count == 1);
  TASK_CHECK(a_eq(&b->insns[3].accesses[0], UD_FRAME_ACCESS_READ,
                  1, 64, UNK, 0));

  /* plain [rbp-0x18] still resolves: rbp == S-8 here */
  TASK_CHECK(b->insns[4].offset == 14);
  TASK_CHECK(a_eq(&b->insns[4].accesses[0], UD_FRAME_ACCESS_READ,
                  1, 64, OFF, -32));
  ud_frame_report_free(rep);

  /* a low-byte alias write (spl) breaks the RSP relation too */
  rep = NULL;
  TASK_CHECK(analyze(code2, sizeof(code2), 0, blocks2, 1, &rep, &err)
             == UD_FRAME_OK);
  b = find_block(rep, 0);
  TASK_CHECK(b != NULL && b->insn_count == 3);
  TASK_CHECK(v_eq(b->insns[1].rsp, OFF, -32));
  TASK_CHECK(v_eq(b->insns[2].rsp, UNK, 0));
  ud_frame_report_free(rep);
  return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "normalize", "invalid_graphs", t_invalid_graphs },
  { "normalize", "decode_errors", t_decode_errors },
  { "normalize", "unreachable_blocks", t_unreachable },
  { "normalize", "reuse_and_ownership", t_reuse_ownership },
  { "normalize", "renumber_stability", t_renumber_stability },
  { "merge", "diamond", t_diamond },
  { "merge", "back_edge", t_back_edge },
  { "merge", "rbp_recovery", t_rbp_recovery },
  { "effects", "prologue_slots", t_prologue_slots },
  { "effects", "push_pop_call", t_push_pop_call },
  { "effects", "pop_rsp", t_pop_rsp },
  { "effects", "alias_and_lea", t_alias_and_lea },
  { "effects", "mov_forms", t_mov_forms },
  { "effects", "signed_imm", t_signed_imm },
  { "effects", "segment_and_alias", t_segment_and_alias },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
