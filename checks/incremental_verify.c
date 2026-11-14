/*
 * Incremental heap-verify checks.
 *
 * These drive the dmalloc_verify_begin()/dmalloc_verify_next()
 * incremental scan against the real allocator.  Only healthy heaps are
 * used: ordinary malloc/realloc/free calls build used, free, and
 * delayed-free slots, and no metadata or user bytes are corrupted on
 * purpose.
 */

#include <stdio.h>
#include <string.h>

#include "dmalloc.h"
#include "debug_tok.h"
#include "error_val.h"

#include "task_support/check.h"

/*
 * Drive a scan to its terminal state.  Asserts the per-call contract,
 * sums the nodes checked, and checks that the terminal state is sticky.
 * Returns the total number of nodes checked through *total_p.
 */
static int	drive_scan(dmalloc_verify_cursor *cursor,
			   const unsigned long budget,
			   unsigned long *total_p)
{
  unsigned long	checked = 0, total = 0, calls = 0;
  int		rc;

  do {
    /* poison the output so we can prove next() always rewrites it */
    checked = 999;
    rc = dmalloc_verify_next(cursor, budget, &checked);
    calls++;
    TASK_CHECK(rc != DMALLOC_SCAN_INVALID);
    TASK_CHECK(rc != DMALLOC_SCAN_STALE);
    TASK_CHECK(rc != DMALLOC_SCAN_ERROR);
    if (rc == DMALLOC_SCAN_MORE) {
      /* every progressing call checks between one and budget nodes */
      TASK_CHECK(checked >= 1);
      TASK_CHECK(checked <= budget);
      total += checked;
    }
    else {
      TASK_CHECK(rc == DMALLOC_SCAN_DONE);
      /* the terminal call itself never checks a node */
      TASK_CHECK(checked == 0);
    }
  } while (rc == DMALLOC_SCAN_MORE && calls < 100000);

  TASK_CHECK(rc == DMALLOC_SCAN_DONE);
  TASK_CHECK(calls < 100000);
  TASK_CHECK(total >= 1);

  /* DONE is terminal: another call must not read any node */
  checked = 999;
  rc = dmalloc_verify_next(cursor, budget, &checked);
  TASK_CHECK(rc == DMALLOC_SCAN_DONE);
  TASK_CHECK(checked == 0);

  *total_p = total;
  return 0;
}

/*
 * Run one progressing call.  A MORE call always checks between one and
 * budget nodes: stage switches are free but never end a call by
 * themselves, so a call either reports DONE or examines at least one
 * node.
 */
static int	step_more(dmalloc_verify_cursor *cursor,
			  const unsigned long budget, unsigned long *checked_p)
{
  unsigned long	checked = 999;
  int		rc;

  rc = dmalloc_verify_next(cursor, budget, &checked);
  TASK_CHECK(rc == DMALLOC_SCAN_MORE);
  TASK_CHECK(checked >= 1);
  TASK_CHECK(checked <= budget);
  *checked_p = checked;
  return 0;
}

/*
 * Run a complete fresh scan with the given budget and return the total
 * number of diagnostic nodes seen.
 */
static int	scan_total(const unsigned long budget, unsigned long *total_p)
{
  dmalloc_verify_cursor	cursor;

  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  return drive_scan(&cursor, budget, total_p);
}

/*
 * Build a heap that contains all four scan stages at once:
 * administration blocks, used slots, real free-list slots (slots that
 * have aged past FREED_POINTER_DELAY), and fresh delayed-free slots.
 */
static int	build_three_phase_heap(void)
{
  void		*keep[6];
  void		*aged[4];
  void		*temp[25];
  int		i;
  static const size_t sizes[6] = { 24, 40, 80, 200, 500, 1000 };

  /* ordinary used allocations in several size bins */
  for (i = 0; i < (int)(sizeof(keep) / sizeof(keep[0])); i++) {
    keep[i] = malloc(sizes[i]);
    TASK_CHECK(keep[i] != NULL);
    memset(keep[i], 0x42, sizes[i]);
  }

  /*
   * Free four same-bin blocks.  They first land on the delayed-free
   * list and then age onto the real free list once enough transactions
   * have passed.  The aging transactions use a different size bin so
   * they never reuse the freed blocks.
   */
  for (i = 0; i < (int)(sizeof(aged) / sizeof(aged[0])); i++) {
    aged[i] = malloc(48);
    TASK_CHECK(aged[i] != NULL);
  }
  for (i = 0; i < (int)(sizeof(aged) / sizeof(aged[0])); i++) {
    free(aged[i]);
  }

  /* more than FREED_POINTER_DELAY (20) transactions in the 128-bin */
  for (i = 0; i < (int)(sizeof(temp) / sizeof(temp[0])); i++) {
    temp[i] = malloc(128);
    TASK_CHECK(temp[i] != NULL);
  }
  for (i = 0; i < (int)(sizeof(temp) / sizeof(temp[0])); i++) {
    free(temp[i]);
  }

  /* fresh frees keep the delayed-free stage non-empty */
  free(keep[4]);
  free(keep[5]);

  return 0;
}

/******************************** budget ********************************/

/* budget of one: every progressing call reports exactly one node */
static int	budget_one_node_per_call(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long	checked = 0, total = 0, calls = 0;
  int			rc;

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);

  do {
    checked = 999;
    rc = dmalloc_verify_next(&cursor, 1, &checked);
    calls++;
    TASK_CHECK(rc == DMALLOC_SCAN_MORE || rc == DMALLOC_SCAN_DONE);
    if (rc == DMALLOC_SCAN_MORE) {
      TASK_CHECK(checked == 1);
      total++;
    }
    else {
      TASK_CHECK(checked == 0);
    }
  } while (rc == DMALLOC_SCAN_MORE && calls < 100000);

  TASK_CHECK(rc == DMALLOC_SCAN_DONE);
  TASK_CHECK(total >= 1);

  /* a large-budget scan over the unchanged heap sees the same nodes */
  {
    unsigned long	big_total = 0;
    TASK_CHECK(scan_total(1000000, &big_total) == 0);
    TASK_CHECK(big_total == total);
  }
  return 0;
}

/* different budgets must visit the same set of nodes exactly once */
static int	budget_totals_agree(void)
{
  static const unsigned long budgets[] = { 1, 2, 5, 13, 64, 1000000 };
  unsigned long		first_total = 0, total = 0;
  unsigned int		i;

  TASK_CHECK(build_three_phase_heap() == 0);

  for (i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++) {
    TASK_CHECK(scan_total(budgets[i], &total) == 0);
    if (i == 0) {
      first_total = total;
    }
    else {
      TASK_CHECK(total == first_total);
    }
  }
  return 0;
}

/* partial progress: while the budget lasts, each call fills it */
static int	budget_partial_progress(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long	checked, cumulative = 0, full_total = 0;
  int			rc, more_calls = 0;

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(scan_total(3, &full_total) == 0);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);

  do {
    checked = 999;
    rc = dmalloc_verify_next(&cursor, 3, &checked);
    if (rc == DMALLOC_SCAN_MORE) {
      more_calls++;
      /*
       * A call checks at most the budget.  It may return fewer when it
       * reaches a level or list boundary: the stage switch itself is
       * free and happens on the next call.
       */
      TASK_CHECK(checked >= 1);
      TASK_CHECK(checked <= 3);
      cumulative += checked;
    }
  } while (rc == DMALLOC_SCAN_MORE && more_calls < 100000);

  TASK_CHECK(rc == DMALLOC_SCAN_DONE);
  TASK_CHECK(more_calls >= 1);
  /* every node was visited exactly once across the progressing calls */
  TASK_CHECK(cumulative == full_total);
  return 0;
}

/* freeing moves a slot between lists without changing the node count */
static int	budget_free_keeps_node_count(void)
{
  void			*p, *q;
  unsigned long		before = 0, after_free = 0, after_alloc = 0;

  p = malloc(64);
  TASK_CHECK(p != NULL);
  q = malloc(64);
  TASK_CHECK(q != NULL);

  /* stabilize all administration blocks before taking the baseline */
  TASK_CHECK(scan_total(7, &before) == 0);

  free(q);					/* used slot -> delayed list */

  TASK_CHECK(scan_total(7, &after_free) == 0);
  /* the same slot is still exactly one node, just in a later stage */
  TASK_CHECK(after_free == before);

  {
    void		*big;
    /*
     * Larger than half a block: no existing divided free slot can
     * satisfy it, so the allocator carves fresh heap blocks and at
     * least one new used slot appears.
     */
    big = malloc(12000);
    TASK_CHECK(big != NULL);
    TASK_CHECK(scan_total(7, &after_alloc) == 0);
    TASK_CHECK(after_alloc >= after_free + 1);
  }
  return 0;
}

/* argument validation: bad calls report INVALID and make no progress */
static int	budget_invalid_arguments(void)
{
  dmalloc_verify_cursor	cursor;
  dmalloc_verify_cursor	junk;
  unsigned long		checked = 999;
  unsigned long	total_a = 0, total_b = 0;

  TASK_CHECK(dmalloc_verify_begin(NULL) == 0);

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);

  /* zero budget */
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 0, &checked) == DMALLOC_SCAN_INVALID);
  TASK_CHECK(checked == 0);

  /* NULL checked output */
  TASK_CHECK(dmalloc_verify_next(&cursor, 1, NULL) == DMALLOC_SCAN_INVALID);

  /* NULL cursor */
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(NULL, 1, &checked) == DMALLOC_SCAN_INVALID);
  TASK_CHECK(checked == 0);

  /* an uninitialized cursor is rejected rather than dereferenced */
  memset(&junk, 0, sizeof(junk));
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&junk, 1, &checked) == DMALLOC_SCAN_INVALID);
  TASK_CHECK(checked == 0);

  /*
   * None of the invalid calls moved the cursor: this same cursor still
   * walks every node exactly once, matching a pristine scan.
   */
  TASK_CHECK(drive_scan(&cursor, 4, &total_a) == 0);
  TASK_CHECK(scan_total(4, &total_b) == 0);
  TASK_CHECK(total_a == total_b);

  /* INVALID on a finished cursor stays INVALID and never revives it */
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 0, &checked) == DMALLOC_SCAN_INVALID);
  TASK_CHECK(checked == 0);
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 4, &checked) == DMALLOC_SCAN_DONE);
  TASK_CHECK(checked == 0);
  return 0;
}

/***************************** invalidation ******************************/

/* a successful allocation invalidates an in-progress scan */
static int	invalidation_on_alloc(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0;
  void			*p;
  int			rc;

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  p = malloc(72);
  TASK_CHECK(p != NULL);

  checked = 999;
  rc = dmalloc_verify_next(&cursor, 2, &checked);
  TASK_CHECK(rc == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  /* STALE is terminal */
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  /* a fresh begin recovers and the stable heap scans cleanly */
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  {
    unsigned long	total = 0;
    TASK_CHECK(drive_scan(&cursor, 5, &total) == 0);
  }
  return 0;
}

/* a successful free invalidates an in-progress scan */
static int	invalidation_on_free(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0;
  void			*p;

  p = malloc(111);
  TASK_CHECK(p != NULL);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  free(p);

  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);
  return 0;
}

/* an in-place shrinking realloc changes the slot and invalidates */
static int	invalidation_on_in_place_realloc(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0;
  void			*p, *q;

  p = malloc(64);
  TASK_CHECK(p != NULL);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  q = realloc(p, 32);
  TASK_CHECK(q == p);			/* same block, shrunk in place */

  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);
  return 0;
}

/* a forced moving realloc invalidates even though the heap stays healthy */
static int	invalidation_on_moved_realloc(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0, flags;
  void			*p, *q;

  flags = dmalloc_debug_current();
  dmalloc_debug(flags | DMALLOC_DEBUG_REALLOC_COPY);

  p = malloc(64);
  TASK_CHECK(p != NULL);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  q = realloc(p, 256);
  TASK_CHECK(q != NULL);
  TASK_CHECK(q != p);			/* copy mode forces the move */

  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  dmalloc_debug(flags);
  return 0;
}

/* requests that fail without touching the heap must not invalidate */
static int	invalidation_failed_requests_keep_scan(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0, total_a = 0, total_b = 0;
  int			stack_marker = 7;
  void			*bad = (void *)&stack_marker;
  void			*p, *r;

  TASK_CHECK(build_three_phase_heap() == 0);

  /* oversized allocation is rejected before any heap change */
  p = malloc(300UL * 1024UL * 1024UL);
  TASK_CHECK(p == NULL);

  /* freeing a non-heap pointer fails and leaves the lists alone */
  dmalloc_errno = DMALLOC_ERROR_NONE;
  free(bad);
  TASK_CHECK(dmalloc_errno != DMALLOC_ERROR_NONE);

  /* realloc of the same bogus pointer fails without moving anything */
  r = realloc(bad, 16);
  TASK_CHECK(r == NULL);

  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  {
    unsigned long	first_checked;
    checked = 999;
    TASK_CHECK(dmalloc_verify_next(&cursor, 3, &checked) == DMALLOC_SCAN_MORE);
    /* a list boundary may end the call before the whole budget is used */
    TASK_CHECK(checked >= 1 && checked <= 3);
    first_checked = checked;

    /* more failed requests in the middle of the scan */
    dmalloc_errno = DMALLOC_ERROR_NONE;
    free(bad);
    TASK_CHECK(dmalloc_errno != DMALLOC_ERROR_NONE);
    TASK_CHECK(realloc(bad, 16) == NULL);
    TASK_CHECK(malloc(300UL * 1024UL * 1024UL) == NULL);

    TASK_CHECK(drive_scan(&cursor, 3, &total_a) == 0);
    TASK_CHECK(scan_total(3, &total_b) == 0);
    /* the failed calls let the original scan finish the full node set */
    TASK_CHECK(total_a + first_checked == total_b);
  }
  return 0;
}

/* real diagnostic configuration changes invalidate; rewrites do not */
static int	invalidation_on_config_change(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0, flags, total = 0;

  flags = dmalloc_debug_current();

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  /* writing the identical value back is not a change */
  dmalloc_debug(flags);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  /* neither is re-applying the same option string */
  dmalloc_debug_setup("debug=0");
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  /* a genuine flag switch invalidates */
  dmalloc_debug(flags | DMALLOC_DEBUG_LOG_TRANS);
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  dmalloc_debug(flags);

  /* a genuine option-string change invalidates too */
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);
  dmalloc_debug_setup("log-trans");
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&cursor, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  dmalloc_debug(flags);
  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(drive_scan(&cursor, 6, &total) == 0);
  return 0;
}

/* queries never invalidate, including the transaction counter itself */
static int	invalidation_queries_are_benign(void)
{
  dmalloc_verify_cursor	cursor;
  unsigned long		checked = 0, mark1, mark2, seen = 0;
  DMALLOC_SIZE		size;
  char			*origin;
  unsigned int		line;
  void			*p;

  p = malloc(88);
  TASK_CHECK(p != NULL);

  TASK_CHECK(dmalloc_verify_begin(&cursor) == 1);
  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);

  /* the mark/transaction counter moves on every API call but is benign */
  mark1 = dmalloc_mark();
  mark2 = dmalloc_mark();
  TASK_CHECK(mark2 >= mark1);

  TASK_CHECK(dmalloc_examine(p, &size, NULL, &origin, &line, NULL, NULL,
			     &seen) == DMALLOC_NOERROR);
  TASK_CHECK(size == 88);
  (void)dmalloc_debug_current();
  TASK_CHECK(dmalloc_verify(p) == DMALLOC_VERIFY_NOERROR);
  /* the full heap check is itself a read-only operation here */
  TASK_CHECK(dmalloc_verify(NULL) == DMALLOC_VERIFY_NOERROR);

  TASK_CHECK(step_more(&cursor, 2, &checked) == 0);
  {
    unsigned long	total = 0;
    TASK_CHECK(drive_scan(&cursor, 5, &total) == 0);
  }
  return 0;
}

/****************************** interleaving *****************************/

/* two cursors walk independently; restarting one leaves the other alone */
static int	interleaving_two_cursors(void)
{
  dmalloc_verify_cursor	a, b;
  unsigned long		checked = 0, total_a = 0, total_b = 0, full = 0;
  unsigned long		b_done = 0;
  int			rc;

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(scan_total(9, &full) == 0);

  TASK_CHECK(dmalloc_verify_begin(&a) == 1);
  TASK_CHECK(dmalloc_verify_begin(&b) == 1);

  TASK_CHECK(step_more(&a, 2, &checked) == 0);
  TASK_CHECK(step_more(&b, 3, &checked) == 0);
  b_done += checked;
  TASK_CHECK(step_more(&a, 2, &checked) == 0);
  TASK_CHECK(step_more(&b, 3, &checked) == 0);
  b_done += checked;

  /* restart a: b's progress must survive untouched */
  TASK_CHECK(dmalloc_verify_begin(&a) == 1);

  TASK_CHECK(drive_scan(&a, 4, &total_a) == 0);
  TASK_CHECK(drive_scan(&b, 4, &total_b) == 0);
  TASK_CHECK(total_a == full);
  /* b had already checked b_done nodes before the interleaving continued */
  TASK_CHECK(total_b + b_done == full);

  /* b is finished and stays finished; a is finished independently too */
  rc = dmalloc_verify_next(&b, 4, &checked);
  TASK_CHECK(rc == DMALLOC_SCAN_DONE);
  TASK_CHECK(checked == 0);
  rc = dmalloc_verify_next(&a, 4, &checked);
  TASK_CHECK(rc == DMALLOC_SCAN_DONE);
  TASK_CHECK(checked == 0);
  return 0;
}

/*
 * A heap change makes the generation stale for every active cursor, but
 * each cursor records its own terminal state and each needs its own
 * fresh begin to recover.
 */
static int	interleaving_invalidation_isolated(void)
{
  dmalloc_verify_cursor	a, b;
  unsigned long		checked = 0, total_a = 0, total_b = 0;
  void			*p;

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(dmalloc_verify_begin(&a) == 1);
  TASK_CHECK(dmalloc_verify_begin(&b) == 1);
  TASK_CHECK(step_more(&a, 2, &checked) == 0);
  TASK_CHECK(step_more(&b, 2, &checked) == 0);

  p = malloc(55);
  TASK_CHECK(p != NULL);

  /* both cursors observe the same generation change independently */
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&a, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);
  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&b, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  /* only a is restarted: b remains terminal while a scans to completion */
  TASK_CHECK(dmalloc_verify_begin(&a) == 1);
  TASK_CHECK(drive_scan(&a, 7, &total_a) == 0);
  TASK_CHECK(total_a >= 1);

  checked = 999;
  TASK_CHECK(dmalloc_verify_next(&b, 2, &checked) == DMALLOC_SCAN_STALE);
  TASK_CHECK(checked == 0);

  /* b recovers only after its own fresh begin */
  TASK_CHECK(dmalloc_verify_begin(&b) == 1);
  TASK_CHECK(drive_scan(&b, 7, &total_b) == 0);
  TASK_CHECK(total_b == total_a);
  return 0;
}

/* interleave scans using different budgets on the same stable heap */
static int	interleaving_mixed_budgets(void)
{
  dmalloc_verify_cursor	a, b;
  unsigned long		checked = 0, total_a = 0, total_b = 0, full = 0;
  unsigned long		a_done = 0, b_done = 0;

  TASK_CHECK(build_three_phase_heap() == 0);
  TASK_CHECK(scan_total(17, &full) == 0);
  TASK_CHECK(full > 17);

  TASK_CHECK(dmalloc_verify_begin(&a) == 1);
  TASK_CHECK(dmalloc_verify_begin(&b) == 1);

  TASK_CHECK(step_more(&a, 1, &checked) == 0);
  a_done += checked;
  TASK_CHECK(step_more(&b, 17, &checked) == 0);
  b_done += checked;
  TASK_CHECK(step_more(&a, 1, &checked) == 0);
  a_done += checked;

  TASK_CHECK(drive_scan(&a, 1, &total_a) == 0);
  TASK_CHECK(drive_scan(&b, 17, &total_b) == 0);

  /* each finished the remaining part of its own independent walk */
  TASK_CHECK(total_a + a_done == full);
  TASK_CHECK(total_b + b_done == full);
  return 0;
}

const struct task_case task_cases[] = {
  /* budget */
  { "budget", "one_node_per_call",	budget_one_node_per_call },
  { "budget", "totals_agree",		budget_totals_agree },
  { "budget", "partial_progress",	budget_partial_progress },
  { "budget", "free_keeps_node_count",	budget_free_keeps_node_count },
  { "budget", "invalid_arguments",	budget_invalid_arguments },
  /* invalidation */
  { "invalidation", "on_alloc",			invalidation_on_alloc },
  { "invalidation", "on_free",			invalidation_on_free },
  { "invalidation", "on_in_place_realloc",	invalidation_on_in_place_realloc },
  { "invalidation", "on_moved_realloc",		invalidation_on_moved_realloc },
  { "invalidation", "failed_requests_keep_scan",
    invalidation_failed_requests_keep_scan },
  { "invalidation", "on_config_change",		invalidation_on_config_change },
  { "invalidation", "queries_are_benign",	invalidation_queries_are_benign },
  /* interleaving */
  { "interleaving", "two_cursors",		interleaving_two_cursors },
  { "interleaving", "invalidation_isolated",
    interleaving_invalidation_isolated },
  { "interleaving", "mixed_budgets",		interleaving_mixed_budgets },
};

const size_t	task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
