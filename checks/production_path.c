/* Proves the production fuzz_one()/fuzz_one_budget() run the six bitflip
   stages through the resumable bitflip_session engine. Apache-2.0. */

#define AFL_TASK_PROBE
#define main afl_original_main
#include "../afl-fuzz.c"
#undef main

#include "../task_support/check.h"
#include "probe_feedback.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define PP_LEN 160

struct pp_event {
  int stage;
  u32 pos;
  u8 data[PP_LEN];
};

static struct pp_event pp_prod_log[8192];
static u32 pp_prod_cnt;
static u64 pp_prod_calls[BFS_STAGES];

static struct pp_event pp_ref_log[8192];
static u32 pp_ref_cnt;
static u64 pp_ref_calls[BFS_STAGES];
static struct bfs_token_ref {
  u8 data[MAX_AUTO_EXTRA];
  u32 len;
} pp_ref_tokens[MAX_AUTO_EXTRAS];
static u32 pp_ref_token_cnt;

static u8 pp_input[PP_LEN];
static u8 pp_failure;
static u8 pp_boundary_map[EFF_ALEN(PP_LEN)];
static u32 pp_boundary_len;
static u8 pp_boundary_seen;

static int pp_stage_id(const u8 *short_name) {

  static const u8 *names[BFS_STAGES] = {
    (u8 *)"flip1", (u8 *)"flip2", (u8 *)"flip4",
    (u8 *)"flip8", (u8 *)"flip16", (u8 *)"flip32"
  };
  int i;

  for (i = 0; i < BFS_STAGES; i++)
    if (!strcmp((char *)short_name, (char *)names[i])) return i;

  return -1;

}

/* Production hook: feeds deterministic coverage and logs every submission. */

static u8 pp_prod_hook(char **argv, u8 *data, u32 len) {

  int stage;

  (void)argv;

  stage = pp_stage_id(stage_short);
  if (stage < 0 || len != PP_LEN || pp_prod_cnt >=
      (sizeof(pp_prod_log) / sizeof(pp_prod_log[0]))) {
    pp_failure = 1;
    stop_soon = 1;
    return 1;
  }

  pp_prod_log[pp_prod_cnt].stage = stage;
  pp_prod_log[pp_prod_cnt].pos   = (u32)stage_cur_byte;
  memcpy(pp_prod_log[pp_prod_cnt].data, data, len);
  pp_prod_cnt++;
  pp_prod_calls[stage]++;

  probe_fill_trace(trace_bits, MAP_SIZE, stage, (u32)stage_cur_byte,
                   data, len, pp_input);
  total_execs++;
  return 0;

}

static void pp_boundary_hook(const u8 *map, u32 len) {

  pp_boundary_seen = 1;
  pp_boundary_len = len;
  memcpy(pp_boundary_map, map, len);
  stop_soon = 1;

}

/* Reference engine hook: same coverage, standalone session. */

static u8 pp_ref_exec(int stage, u32 pos, const u8 *cand, u32 len,
                      u32 *cksum_out, void *userdata) {

  static u8 ref_map[MAP_SIZE];

  (void)userdata;

  if (pp_ref_cnt >= sizeof(pp_ref_log) / sizeof(pp_ref_log[0])) return 1;

  pp_ref_log[pp_ref_cnt].stage = stage;
  pp_ref_log[pp_ref_cnt].pos   = pos;
  memcpy(pp_ref_log[pp_ref_cnt].data, cand, len);
  pp_ref_cnt++;
  pp_ref_calls[stage]++;

  probe_fill_trace(ref_map, MAP_SIZE, stage, pos, cand, len, pp_input);
  *cksum_out = hash32(ref_map, MAP_SIZE, HASH_CONST);
  return 0;

}

static void pp_ref_token(const u8 *token, u32 len, void *userdata) {

  (void)userdata;

  if (pp_ref_token_cnt >= MAX_AUTO_EXTRAS) return;
  memcpy(pp_ref_tokens[pp_ref_token_cnt].data, token, len);
  pp_ref_tokens[pp_ref_token_cnt].len = len;
  pp_ref_token_cnt++;

}

static void pp_reset_extras(void) {

  u32 i;

  for (i = 0; i < a_extras_cnt; i++) ck_free(a_extras[i].data);
  ck_free(a_extras);
  a_extras = NULL;
  a_extras_cnt = 0;

}

int production_path_run(void) {

  unsigned i;
  int fd;
  const char *file = ".task-build/pp-input";
  struct queue_entry q;
  /* Uneven budgets, including repeats of 1 and 0, ending with unlimited. */
  static const u64 budgets[] = { 1, 1, 7, 0, 13, 100, 257, 1000, 4096,
                                 (u64)-1 };
  u8 rc;
  struct bitflip_session *ref;
  u32 eff_len, k;
  const u8 *eff_map;
  u32 prod_token_cnt;
  struct extra_data *prod_tokens;

  for (i = 0; i < PP_LEN; i++) pp_input[i] = (u8)('A' + (i * 7) % 26);

  fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  TASK_CHECK(fd >= 0);
  TASK_CHECK(write(fd, pp_input, PP_LEN) == PP_LEN);
  close(fd);

  trace_bits = calloc(1, MAP_SIZE);
  TASK_CHECK(trace_bits != NULL);

  memset(&q, 0, sizeof(q));
  q.fname = (u8 *)file;
  q.len = PP_LEN;
  q.depth = 1;
  q.favored = 1;
  q.trim_done = 1;
  q.exec_us = 1;
  q.bitmap_size = 1;
  q.exec_cksum = hash32(trace_bits, MAP_SIZE, HASH_CONST);

  queue_cur = queue = &q;
  queued_paths = 1;
  pending_not_fuzzed = 1;
  pending_favored = 0;
  total_cal_us = total_cal_cycles = 1;
  total_bitmap_size = total_bitmap_entries = 1;

  /* Reference: the standalone engine driven with an infinite budget. */

  ref = bitflip_session_open(pp_input, PP_LEN, q.exec_cksum, 0, pp_ref_exec,
                             pp_ref_token, NULL, NULL);
  TASK_CHECK(ref != NULL);
  TASK_CHECK(bitflip_session_run(ref, (u64)-1) == BFS_COMPLETE);

  eff_map = bitflip_session_eff_map(ref, &eff_len);
  TASK_CHECK(eff_len == EFF_ALEN(PP_LEN));

  {
    static u8 ref_eff[EFF_ALEN(PP_LEN)];
    memcpy(ref_eff, eff_map, eff_len);

    /* Production: fuzz_one_budget() driven in deliberately uneven slices. */

    afl_task_execute_hook = pp_prod_hook;
    afl_task_boundary_hook = pp_boundary_hook;

    /* A zero budget executes nothing and must not mark the entry fuzzed. */

    rc = fuzz_one_budget(NULL, 0);
    TASK_CHECK(rc == 2);
    TASK_CHECK(pp_prod_cnt == 0);
    TASK_CHECK(!q.was_fuzzed && !q.passed_det);
    TASK_CHECK(q.bitflip_sess != NULL);
    TASK_CHECK(bitflip_session_status(q.bitflip_sess) == BFS_PAUSED);

    for (i = 0; i < sizeof(budgets) / sizeof(budgets[0]) && !stop_soon; i++) {

      u32 before = pp_prod_cnt;

      rc = fuzz_one_budget(NULL, budgets[i]);

      if (budgets[i] == 0) {
        TASK_CHECK(rc == 2);
        TASK_CHECK(pp_prod_cnt == before);     /* nothing billed           */
        continue;
      }

      if (!stop_soon) {
        TASK_CHECK(rc == 2);                   /* paused mid-prelude       */
        TASK_CHECK(!q.was_fuzzed && !q.passed_det);
        TASK_CHECK(q.bitflip_sess != NULL);
      }

    }

    /* The prelude finished inside this fuzz_one and the boundary hook
       stopped the run before arithmetic/havoc. */

    TASK_CHECK(stop_soon == 1);
    TASK_CHECK(pp_boundary_seen == 1);
    TASK_CHECK(q.bitflip_sess == NULL);

    /* Same candidate sequence and per-stage exec counts as the reference. */

    TASK_CHECK(pp_prod_cnt == pp_ref_cnt);
    TASK_CHECK(pp_prod_cnt > 0);

    for (k = 0; k < pp_prod_cnt; k++) {
      TASK_CHECK(pp_prod_log[k].stage == pp_ref_log[k].stage);
      TASK_CHECK(pp_prod_log[k].pos == pp_ref_log[k].pos);
      TASK_CHECK(!memcmp(pp_prod_log[k].data, pp_ref_log[k].data, PP_LEN));
    }

    for (i = 0; i < BFS_STAGES; i++) {
      TASK_CHECK(pp_prod_calls[i] == pp_ref_calls[i]);
      TASK_CHECK((u64)stage_cycles[i] == pp_prod_calls[i]);
    }

    /* The effector map handed to later stages matches the reference. */

    TASK_CHECK(pp_boundary_len == eff_len);
    TASK_CHECK(!memcmp(pp_boundary_map, ref_eff, eff_len));

    /* The production dictionary is what maybe_add_auto produces from the
       reference token proposals (filtering/dedup/sort included). */

    prod_token_cnt = a_extras_cnt;
    prod_tokens = ck_memdup(a_extras,
                            prod_token_cnt * sizeof(struct extra_data));
    for (i = 0; i < prod_token_cnt; i++)
      prod_tokens[i].data = ck_memdup(a_extras[i].data, a_extras[i].len);

    pp_reset_extras();
    for (i = 0; i < pp_ref_token_cnt; i++)
      maybe_add_auto(pp_ref_tokens[i].data, pp_ref_tokens[i].len);

    TASK_CHECK(a_extras_cnt == prod_token_cnt);
    for (i = 0; i < prod_token_cnt; i++) {
      TASK_CHECK(a_extras[i].len == prod_tokens[i].len);
      TASK_CHECK(!memcmp(a_extras[i].data, prod_tokens[i].data,
                         prod_tokens[i].len));
      ck_free(prod_tokens[i].data);
    }
    ck_free(prod_tokens);

    TASK_CHECK(!pp_failure);
  }

  pp_reset_extras();
  free(trace_bits);
  trace_bits = NULL;
  unlink(file);

  return 0;

}

/* The infinite-budget fuzz_one() wrapper: the whole six-stage prelude must
   finish in a single call, going through the same session machinery. */

int production_full_run(void) {

  unsigned i;
  int fd;
  const char *file = ".task-build/pp-input-full";
  struct queue_entry q;
  struct bitflip_session *ref;
  u32 eff_len, k;
  const u8 *eff_map;
  static u8 ref_eff[EFF_ALEN(PP_LEN)];

  pp_prod_cnt = 0;
  pp_ref_cnt = 0;
  pp_ref_token_cnt = 0;
  pp_boundary_seen = 0;
  pp_failure = 0;
  stop_soon = 0;

  for (i = 0; i < BFS_STAGES; i++) pp_prod_calls[i] = pp_ref_calls[i] = 0;

  for (i = 0; i < PP_LEN; i++) pp_input[i] = (u8)('K' + (i * 5) % 21);

  fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  TASK_CHECK(fd >= 0);
  TASK_CHECK(write(fd, pp_input, PP_LEN) == PP_LEN);
  close(fd);

  trace_bits = calloc(1, MAP_SIZE);
  TASK_CHECK(trace_bits != NULL);

  memset(&q, 0, sizeof(q));
  q.fname = (u8 *)file;
  q.len = PP_LEN;
  q.depth = 1;
  q.favored = 1;
  q.trim_done = 1;
  q.exec_us = 1;
  q.bitmap_size = 1;
  q.exec_cksum = hash32(trace_bits, MAP_SIZE, HASH_CONST);

  queue_cur = queue = &q;
  queued_paths = 1;
  pending_not_fuzzed = 1;
  pending_favored = 0;
  total_cal_us = total_cal_cycles = 1;
  total_bitmap_size = total_bitmap_entries = 1;

  ref = bitflip_session_open(pp_input, PP_LEN, q.exec_cksum, 0, pp_ref_exec,
                             pp_ref_token, NULL, NULL);
  TASK_CHECK(ref != NULL);
  TASK_CHECK(bitflip_session_run(ref, (u64)-1) == BFS_COMPLETE);
  eff_map = bitflip_session_eff_map(ref, &eff_len);
  memcpy(ref_eff, eff_map, eff_len);
  bitflip_session_close(ref);

  afl_task_execute_hook = pp_prod_hook;
  afl_task_boundary_hook = pp_boundary_hook;

  /* A single, unbounded fuzz_one() call drives the entire prelude. */

  TASK_CHECK(fuzz_one(NULL) == 1);   /* boundary hook stops the entry        */
  TASK_CHECK(stop_soon == 1);
  TASK_CHECK(pp_boundary_seen == 1);
  TASK_CHECK(q.bitflip_sess == NULL);

  TASK_CHECK(pp_prod_cnt == pp_ref_cnt);
  for (k = 0; k < pp_prod_cnt; k++) {
    TASK_CHECK(pp_prod_log[k].stage == pp_ref_log[k].stage);
    TASK_CHECK(pp_prod_log[k].pos == pp_ref_log[k].pos);
    TASK_CHECK(!memcmp(pp_prod_log[k].data, pp_ref_log[k].data, PP_LEN));
  }

  for (i = 0; i < BFS_STAGES; i++)
    TASK_CHECK(pp_prod_calls[i] == pp_ref_calls[i]);

  TASK_CHECK(pp_boundary_len == eff_len);
  TASK_CHECK(!memcmp(pp_boundary_map, ref_eff, eff_len));
  TASK_CHECK(!pp_failure);

  afl_task_execute_hook = NULL;
  afl_task_boundary_hook = NULL;

  pp_reset_extras();
  free(trace_bits);
  trace_bits = NULL;
  unlink(file);

  return 0;

}
