/**********************************************************************
  history_contract.c - model driven capture-history regression (contract)

  The contract builds finite call plans from the independent model in
  history_model.c, executes them against Oniguruma, and checks every
  observable projection against the model:

    * group 0 range and every ordinary named capture (byte offsets),
    * every capture-history node: group, beg, end, parent/child links and
      sibling order,
    * the full BOTH traversal event stream (entry, exit, level, order),
    * match-vs-no-match for the corrupted-tail control cases.

  Nothing here feeds Oniguruma output back into the model: expectations come
  from the model before the search is run.

  Entry point (no main in this file):
    int history_contract(const char *suite, unsigned seed, int case_id);

  Returns the number of positive assertions actually executed, or a negative
  value on the first concrete deviation / unsupported suite.
**********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oniguruma.h"
#include "history_model.c"

#define HC_MAX_FLAT   HM_MAX_HIST
#define HC_MAX_EV     HM_MAX_EV
#define HC_ERR_MAX    512

typedef struct hc_flat_st {
  int group, beg, end, parent, depth;
} hc_flat;

typedef struct hc_evt_st {
  int group, beg, end, depth, at;
} hc_evt;

typedef struct hc_ctx_st {
  int      assertions;
  int      failed;
  char     err[HC_ERR_MAX];

  hc_flat  flat[HC_MAX_FLAT];
  int      nflat;
  hc_evt   evts[HC_MAX_EV];
  int      nevts;
  int      evt_overflow;

  const hm_plan *pl;
} hc_ctx;

static void
hc_fail(hc_ctx *c, const char *fmt, ...)
{
  va_list ap;
  if (c->failed) return; /* keep the first concrete difference only */
  c->failed = 1;
  va_start(ap, fmt);
  vsnprintf(c->err, sizeof(c->err), fmt, ap);
  va_end(ap);
}

#define HC_CHECK(c, cond, ...) do {                 \
    (c)->assertions++;                              \
    if (!(cond)) { hc_fail((c), __VA_ARGS__); }     \
  } while (0)

/* ------------------------------ Oniguruma glue --------------------------- */

static int hc_onig_ready = 0;

static int
hc_onig_init(void)
{
  if (hc_onig_ready) return 0;
  OnigEncoding enc = ONIG_ENCODING_ASCII;
  if (onig_initialize(&enc, 1) != 0) return -1;
  hc_onig_ready = 1;
  return 0;
}

static int
hc_flatten(hc_ctx *c, const OnigCaptureTreeNode *node, int parent, int depth)
{
  int idx;
  if (!node) { hc_fail(c, "actual history tree has a null node"); return -1; }
  if (c->nflat >= HC_MAX_FLAT) {
    hc_fail(c, "actual history has more than %d nodes", HC_MAX_FLAT);
    return -1;
  }
  idx = c->nflat++;
  c->flat[idx].group = node->group;
  c->flat[idx].beg   = node->beg;
  c->flat[idx].end   = node->end;
  c->flat[idx].parent = parent;
  c->flat[idx].depth  = depth;

  for (int i = 0; i < node->num_childs; i++) {
    if (hc_flatten(c, node->childs[i], idx, depth + 1) != 0) return -1;
  }
  return 0;
}

static int
hc_traverse_cb(int group, int beg, int end, int level, int at, void *arg)
{
  hc_ctx *c = (hc_ctx *)arg;
  if (c->nevts >= HC_MAX_EV) { c->evt_overflow = 1; return 0; }
  c->evts[c->nevts].group = group;
  c->evts[c->nevts].beg   = beg;
  c->evts[c->nevts].end   = end;
  c->evts[c->nevts].depth = level;
  c->evts[c->nevts].at    = at;
  c->nevts++;
  return 0;
}

/* Verify every projection of one model plan against one real search. */
static int
hc_verify_plan(const hm_plan *pl, unsigned seed, int case_id, const char *tag)
{
  hc_ctx c;
  OnigSyntaxType syn;
  OnigErrorInfo ei;
  OnigRegex re = NULL;
  OnigRegion *region = NULL;
  OnigCaptureTreeNode *root;
  int rc;

  memset(&c, 0, sizeof(c));
  c.pl = pl;

  if (hc_onig_init() != 0) {
    hc_fail(&c, "onig_initialize failed");
    goto report;
  }

  onig_copy_syntax(&syn, ONIG_SYNTAX_DEFAULT);
  onig_set_syntax_op2(&syn,
      onig_get_syntax_op2(&syn) | ONIG_SYN_OP2_ATMARK_CAPTURE_HISTORY);

  rc = onig_new(&re, (const UChar *)pl->pattern,
                (const UChar *)(pl->pattern + strlen(pl->pattern)),
                0, ONIG_ENCODING_ASCII, &syn, &ei);
  HC_CHECK(&c, rc == 0, "pattern compile failed rc=%d", rc);
  if (rc != 0) goto report;

  /* declaration table: named groups must carry the model's numbers */
  for (int i = 0; i < pl->ndecl; i++) {
    const hm_decl *d = &pl->decl[i];
    int *nums = NULL;
    int ng = onig_name_to_group_numbers(re,
                  (const UChar *)d->name,
                  (const UChar *)(d->name + strlen(d->name)), &nums);
    HC_CHECK(&c, ng == 1 && nums && nums[0] == d->number,
             "name '%s' resolves to group %d but model says %d",
             d->name, (ng == 1 && nums) ? nums[0] : -1, d->number);
  }

  region = onig_region_new();
  HC_CHECK(&c, region != NULL, "onig_region_new returned null");
  if (!region) goto report;

  rc = onig_search(re, (const UChar *)pl->input,
                   (const UChar *)(pl->input + pl->ilen),
                   (const UChar *)pl->input,
                   (const UChar *)(pl->input + pl->ilen), region, 0);

  if (!pl->expect_match) {
    HC_CHECK(&c, rc < 0,
             "corrupted-tail control must not match, but rc=%d", rc);
    goto report;
  }

  HC_CHECK(&c, rc >= 0, "expected match but search failed rc=%d", rc);
  if (rc < 0) goto report;

  /* ordinary captures, including group 0 */
  HC_CHECK(&c, region->num_regs == pl->nregs,
           "region has %d regs but model declares %d",
           region->num_regs, pl->nregs);
  for (int i = 0; i < pl->nregs && i < region->num_regs; i++) {
    HC_CHECK(&c, region->beg[i] == pl->reg[i].beg,
             "reg[%d] beg actual=%d model=%d",
             i, region->beg[i], pl->reg[i].beg);
    HC_CHECK(&c, region->end[i] == pl->reg[i].end,
             "reg[%d] end actual=%d model=%d",
             i, region->end[i], pl->reg[i].end);
  }

  /* capture-history tree structure */
  root = onig_get_capture_tree(region);
  HC_CHECK(&c, root != NULL, "expected a capture-history tree");
  if (root) {
    if (hc_flatten(&c, root, -1, 0) == 0) {
      HC_CHECK(&c, c.nflat == pl->nhist,
               "history node count actual=%d model=%d", c.nflat, pl->nhist);
      int lim = c.nflat < pl->nhist ? c.nflat : pl->nhist;
      for (int i = 0; i < lim; i++) {
        HC_CHECK(&c, c.flat[i].group == pl->hist[i].group,
                 "hist[%d] group actual=%d model=%d",
                 i, c.flat[i].group, pl->hist[i].group);
        HC_CHECK(&c, c.flat[i].beg == pl->hist[i].beg,
                 "hist[%d] beg actual=%d model=%d",
                 i, c.flat[i].beg, pl->hist[i].beg);
        HC_CHECK(&c, c.flat[i].end == pl->hist[i].end,
                 "hist[%d] end actual=%d model=%d",
                 i, c.flat[i].end, pl->hist[i].end);
        HC_CHECK(&c, c.flat[i].parent == pl->hist[i].parent,
                 "hist[%d] parent actual=%d model=%d",
                 i, c.flat[i].parent, pl->hist[i].parent);
        HC_CHECK(&c, c.flat[i].depth == pl->hist[i].depth,
                 "hist[%d] depth actual=%d model=%d",
                 i, c.flat[i].depth, pl->hist[i].depth);
      }
    }
  }

  /* BOTH traversal events, expanded independently from model instances */
  rc = onig_capture_tree_traverse(region, ONIG_TRAVERSE_CALLBACK_AT_BOTH,
                                  hc_traverse_cb, &c);
  HC_CHECK(&c, rc == 0, "capture_tree_traverse returned %d", rc);
  HC_CHECK(&c, !c.evt_overflow, "traversal produced too many events");
  HC_CHECK(&c, c.nevts == pl->nevents,
           "event count actual=%d model=%d", c.nevts, pl->nevents);
  {
    int lim = c.nevts < pl->nevents ? c.nevts : pl->nevents;
    for (int i = 0; i < lim; i++) {
      HC_CHECK(&c, c.evts[i].group == pl->events[i].group &&
                   c.evts[i].beg == pl->events[i].beg &&
                   c.evts[i].end == pl->events[i].end &&
                   c.evts[i].depth == pl->events[i].depth &&
                   c.evts[i].at == pl->events[i].at,
               "event[%d] actual=(g%d %d,%d lvl%d at%d) "
               "model=(g%d %d,%d lvl%d at%d)",
               i, c.evts[i].group, c.evts[i].beg, c.evts[i].end,
               c.evts[i].depth, c.evts[i].at,
               pl->events[i].group, pl->events[i].beg, pl->events[i].end,
               pl->events[i].depth, pl->events[i].at);
    }
  }

report:
  if (region) onig_region_free(region, 1);
  if (re) onig_free(re);

  if (c.failed) {
    fprintf(stderr,
      "==== HISTORY CONTRACT DIFFERENCE ====\n"
      "seed=%u case=%d suite-tag=%s\n%s"
      "first difference: %s\n"
      "pattern: %s\ninput(%d): %s\n"
      "rerun: target --suite <suite> --case %d --seed %u\n"
      "======================================\n",
      seed, case_id, tag, pl->text, c.err, pl->pattern, pl->ilen,
      pl->input, case_id, seed);
    return -1;
  }
  return c.assertions;
}

/* Run one plan twice (probe-test vs direct control) and require identical
 * winning projections: a retracted completed call must leave no trace. */
static int
hc_verify_branch_pair(unsigned seed, int case_id, int wrapped)
{
  hm_plan test, control;
  int n1, n2;

  hm_plan_build(&test, HM_KIND_BRANCH_TEST, wrapped, seed, case_id);
  hm_plan_build(&control, HM_KIND_BRANCH_CONTROL, wrapped, seed, case_id);

  n1 = hc_verify_plan(&test, seed, case_id,
                      wrapped ? "branch-test/wrapped" : "branch-test");
  if (n1 < 0) return n1;
  n2 = hc_verify_plan(&control, seed, case_id,
                      wrapped ? "branch-control/wrapped" : "branch-control");
  if (n2 < 0) return n2;

  if (test.expect_match) {
    if (test.winner != control.winner ||
        test.nhist != control.nhist ||
        test.nevents != control.nevents ||
        test.nregs != control.nregs ||
        memcmp(test.reg, control.reg,
               (size_t)test.nregs * sizeof(hm_range)) != 0 ||
        memcmp(test.hist, control.hist,
               (size_t)test.nhist * sizeof(hm_hnode)) != 0) {
      fprintf(stderr,
        "==== HISTORY CONTRACT DIFFERENCE ====\n"
        "seed=%u case=%d tag=branch-pair wrapped=%d\n"
        "failed probe call changed the winning projection vs control\n"
        "rerun: target --suite branch --case %d --seed %u\n"
        "======================================\n",
        seed, case_id, wrapped, case_id, seed);
      return -1;
    }
    n1++; /* pair-equivalence assertion */
  }
  return n1 + n2;
}

/* ------------------------------- suite entry ----------------------------- */

int
history_contract(const char *suite, unsigned seed, int case_id)
{
  hm_plan pl;
  int total = 0;
  int n;

  if (!suite) return -2;

  if (strcmp(suite, "model") == 0) {
    char err[HC_ERR_MAX];
    n = hm_selfcheck(case_id, seed, err, sizeof(err));
    if (n <= 0) {
      fprintf(stderr,
        "==== HISTORY CONTRACT DIFFERENCE ====\n"
        "seed=%u case=%d suite=model\nfirst difference: %s\n"
        "rerun: target --suite model --case %d --seed %u\n"
        "======================================\n",
        seed, case_id, err[0] ? err : "empty model selfcheck",
        case_id, seed);
      return -1;
    }
    return n;
  }

  if (strcmp(suite, "branch") == 0) {
    /* unwrapped and wrapper-renumbered, test plus direct control */
    n = hc_verify_branch_pair(seed, case_id, 0);
    if (n < 0) return n;
    total += n;
    n = hc_verify_branch_pair(seed, case_id, 1);
    if (n < 0) return n;
    total += n;
    return total;
  }

  if (strcmp(suite, "repeat") == 0) {
    /* greedy repeat, then the combined probe-in-repeat construction */
    for (int kind = HM_KIND_REPEAT; kind <= HM_KIND_COMBINED; kind++) {
      for (int wrapped = 0; wrapped < 2; wrapped++) {
        hm_plan_build(&pl, kind, wrapped, seed, case_id);
        n = hc_verify_plan(&pl, seed, case_id,
                           kind == HM_KIND_REPEAT ? "repeat" : "combined");
        if (n < 0) return n;
        total += n;
      }
    }
    return total;
  }

  if (strcmp(suite, "projection") == 0) {
    /* every plan kind, both declaration tables: full node + event projection */
    for (int kind = 0; kind < 4; kind++) {
      for (int wrapped = 0; wrapped < 2; wrapped++) {
        hm_plan_build(&pl, kind, wrapped, seed, case_id);
        n = hc_verify_plan(&pl, seed, case_id, "projection");
        if (n < 0) return n;
        total += n;
      }
    }
    return total;
  }

  fprintf(stderr, "unsupported suite '%s'\n", suite);
  return -2;
}
