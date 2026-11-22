/*
  Resume contract regression for the resumable deterministic bitflip
  prelude. Two groups are registered:

    slice_feedback     - one-shot vs budget-sliced driving, zero budgets,
                         failed-candidate retry, interleaved sessions,
                         short inputs, and the production fuzz_one() path
    checkpoint_handoff - save/load across stage boundaries, a real
                         cross-process handoff, corruption/identity rejection
                         and no-replay of completed sessions

  All feedback is synthetic, finite and deterministic; no third-party target
  runs and no crashing input is generated. Apache-2.0.
*/

#include "../task_support/check.h"
#include "../bitflip_session.h"
#include "../hash.h"
#include "probe_feedback.h"
#include "production_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ */
/* Shared observation harness                                         */
/* ------------------------------------------------------------------ */

#define RC_LEN       200
#define RC_LOG_CAP   20000

struct rc_event {
  int stage;
  u32 pos;
  u8 cand[RC_LEN];
};

struct rc_obs {
  struct rc_event log[RC_LOG_CAP];
  u32 count;
  u64 calls[BFS_STAGES];
  u64 token_calls;
  u64 event_calls;
  struct {
    u8 data[MAX_AUTO_EXTRA];
    u32 len;
  } tokens[MAX_AUTO_EXTRAS];
  u32 token_cnt;
  const u8 *input;
  u32 input_len;
  u8 map[MAP_SIZE];
  /* When non-NULL, submission attempts at these ordinals fail once. */
  const u64 *fail_at;
  u32 fail_cnt;
  u64 attempts;
  u32 fail_delivered;
};

static u8 rc_exec_cb(int stage, u32 pos, const u8 *cand, u32 len,
                     u32 *cksum_out, void *userdata) {

  struct rc_obs *o = (struct rc_obs *)userdata;
  u32 i;

  if (o->count >= RC_LOG_CAP) return 1;

  /* Deterministic injected failure: the candidate must not be logged or
     billed, and a later call retries exactly this candidate. */

  for (i = 0; i < o->fail_cnt; i++) {
    if (o->fail_at[i] == o->attempts) {
      o->attempts++;
      o->fail_delivered++;
      return 1;
    }
  }

  o->attempts++;

  o->log[o->count].stage = stage;
  o->log[o->count].pos = pos;
  memcpy(o->log[o->count].cand, cand, len);
  o->count++;
  o->calls[stage]++;

  probe_fill_trace(o->map, MAP_SIZE, stage, pos, cand, len, o->input);
  *cksum_out = hash32(o->map, MAP_SIZE, HASH_CONST);
  return 0;

}

static void rc_token_cb(const u8 *token, u32 len, void *userdata) {

  struct rc_obs *o = (struct rc_obs *)userdata;

  o->token_calls++;
  if (o->token_cnt >= MAX_AUTO_EXTRAS) return;
  memcpy(o->tokens[o->token_cnt].data, token, len);
  o->tokens[o->token_cnt].len = len;
  o->token_cnt++;

}

static void rc_event_cb(int event, int stage, u64 a, u64 b, void *userdata) {

  struct rc_obs *o = (struct rc_obs *)userdata;
  (void)event; (void)stage; (void)a; (void)b;
  o->event_calls++;

}

static void rc_make_input(u8 *buf, u32 len, u8 seed) {

  u32 i;

  for (i = 0; i < len; i++)
    buf[i] = (u8)(seed + (i * 31 + i / 7) * 13 + (i ^ 0x5a));

}

#define RC_EFF_ALEN(_l) (((_l) >> EFF_MAP_SCALE2) + \
                         !!((_l) & ((1 << EFF_MAP_SCALE2) - 1)))

/* Reference result of one complete, unlimited run. */

struct rc_reference {
  struct rc_obs obs;
  u8 eff[RC_LEN / 8 + 1];
  u32 eff_len;
  u8 input[RC_LEN];
  u32 base_cksum;
};

static void rc_obs_init(struct rc_obs *o, const u8 *input, u32 len) {

  memset(o, 0, sizeof(*o));
  o->input = input;
  o->input_len = len;

}

static int rc_build_reference(struct rc_reference *r, u8 dumb_mode) {

  struct bitflip_session *s;

  memset(r, 0, sizeof(*r));
  rc_make_input(r->input, RC_LEN, 7);
  r->base_cksum = 0;                 /* zero map hashes to a known value    */
  rc_obs_init(&r->obs, r->input, RC_LEN);

  s = bitflip_session_open(r->input, RC_LEN, r->base_cksum, dumb_mode,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &r->obs);
  TASK_CHECK(s != NULL);
  TASK_CHECK(bitflip_session_run(s, (u64)-1) == BFS_COMPLETE);
  TASK_CHECK(bitflip_session_status(s) == BFS_COMPLETE);

  r->eff_len = 0;
  memcpy(r->eff, bitflip_session_eff_map(s, &r->eff_len), r->eff_len);
  TASK_CHECK(r->eff_len == RC_EFF_ALEN(RC_LEN));

  bitflip_session_close(s);
  return 0;

}

/* Compare two full observation logs, token streams and effector maps. */

static int rc_compare(const struct rc_reference *r,
                      const struct rc_obs *o, const u8 *eff, u32 eff_len) {

  u32 i;

  TASK_CHECK(o->count == r->obs.count);
  TASK_CHECK(o->count > 0);

  for (i = 0; i < o->count; i++) {
    TASK_CHECK(o->log[i].stage == r->obs.log[i].stage);
    TASK_CHECK(o->log[i].pos == r->obs.log[i].pos);
    TASK_CHECK(!memcmp(o->log[i].cand, r->obs.log[i].cand, RC_LEN));
  }

  for (i = 0; i < BFS_STAGES; i++)
    TASK_CHECK(o->calls[i] == r->obs.calls[i]);

  TASK_CHECK(o->token_cnt == r->obs.token_cnt);
  TASK_CHECK(o->token_calls == r->obs.token_calls);
  for (i = 0; i < o->token_cnt; i++) {
    TASK_CHECK(o->tokens[i].len == r->obs.tokens[i].len);
    TASK_CHECK(!memcmp(o->tokens[i].data, r->obs.tokens[i].data,
                       o->tokens[i].len));
  }

  TASK_CHECK(eff_len == r->eff_len);
  TASK_CHECK(!memcmp(eff, r->eff, eff_len));
  return 0;

}

/* Drive a fresh session to completion using the given budget schedule and
   return the full observation plus final effector map. */

static int rc_run_sliced(const struct rc_reference *r, const u64 *budgets,
                         u32 budget_cnt, struct rc_obs *o, u8 *eff,
                         u32 *eff_len, u8 dumb_mode,
                         const u64 *fail_at, u32 fail_cnt) {

  struct bitflip_session *s;
  u32 i;
  u64 billed_so_far = 0;

  rc_obs_init(o, r->input, RC_LEN);
  o->fail_at = fail_at;
  o->fail_cnt = fail_cnt;

  s = bitflip_session_open(r->input, RC_LEN, r->base_cksum, dumb_mode,
                           rc_exec_cb, rc_token_cb, rc_event_cb, o);
  TASK_CHECK(s != NULL);

  for (i = 0; i < budget_cnt; i++) {

    u64 before = o->count;
    int rc = bitflip_session_run(s, budgets[i]);

    if (budgets[i] == 0) {

      /* Zero budget: nothing submitted, still paused. */

      TASK_CHECK(o->count == before);
      TASK_CHECK(rc == BFS_PAUSED);
      continue;

    }

    /* Only actually submitted candidates are billed. */

    TASK_CHECK(o->count - before <= budgets[i]);
    billed_so_far += o->count - before;

    if (rc == BFS_COMPLETE) break;
    TASK_CHECK(rc == BFS_PAUSED || rc == BFS_ERROR);

    /* After an injected failure the same candidate is retried on the next
       call, so no log entry was consumed by the failed attempt. */

    if (rc == BFS_ERROR) {
      TASK_CHECK(bitflip_session_status(s) == BFS_ERROR);
      /* Re-arm: clear failure list by marking it exhausted via a fresh
         run; the session stays at the same stage/position. */
    }

  }

  TASK_CHECK(billed_so_far == o->count);
  TASK_CHECK(bitflip_session_status(s) == BFS_COMPLETE);

  *eff_len = 0;
  memcpy(eff, bitflip_session_eff_map(s, eff_len), *eff_len);
  bitflip_session_close(s);
  return 0;

}

/* ------------------------------------------------------------------ */
/* slice_feedback cases                                               */
/* ------------------------------------------------------------------ */

static int case_full_vs_sliced(void) {

  static struct rc_reference ref;
  static struct rc_obs sliced;
  static u8 eff[RC_LEN / 8 + 1];
  u32 eff_len;
  static const u64 budgets[] = {
    1, 1, 7, 3, 13, 0, 100, 257, 1, 1, 1000, 4096, 9999, (u64)-1
  };

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);

  TASK_CHECK(rc_run_sliced(&ref, budgets,
                           sizeof(budgets) / sizeof(budgets[0]),
                           &sliced, eff, &eff_len, 0, NULL, 0) == 0);

  return rc_compare(&ref, &sliced, eff, eff_len);

}

static int case_alternate_budget_schedule(void) {

  static struct rc_reference ref;
  static struct rc_obs sliced;
  static u8 eff[RC_LEN / 8 + 1];
  u32 eff_len;
  /* Prime-ish schedule that repeatedly lands mid-stage. */
  static const u64 budgets[] = {
    11, 23, 47, 97, 199, 401, 809, 1597, 3203, 6401, (u64)-1
  };

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);
  TASK_CHECK(rc_run_sliced(&ref, budgets,
                           sizeof(budgets) / sizeof(budgets[0]),
                           &sliced, eff, &eff_len, 0, NULL, 0) == 0);
  return rc_compare(&ref, &sliced, eff, eff_len);

}

static int case_zero_budget_is_free(void) {

  static struct rc_reference ref;
  static struct rc_obs o;
  struct bitflip_session *s;

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);
  rc_obs_init(&o, ref.input, RC_LEN);

  s = bitflip_session_open(ref.input, RC_LEN, ref.base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &o);
  TASK_CHECK(s != NULL);

  TASK_CHECK(bitflip_session_run(s, 0) == BFS_PAUSED);
  TASK_CHECK(bitflip_session_run(s, 0) == BFS_PAUSED);
  TASK_CHECK(o.count == 0);
  TASK_CHECK(bitflip_session_stage(s) == BFS_FLIP1);
  TASK_CHECK(bitflip_session_stage_index(s) == 0);
  TASK_CHECK(bitflip_session_execs(s) == 0);

  bitflip_session_close(s);
  return 0;

}

static int case_failed_candidate_retries(void) {

  static struct rc_reference ref;
  static struct rc_obs o;
  static u8 eff[RC_LEN / 8 + 1];
  u32 eff_len;
  struct bitflip_session *s;
  /* Fail the 5th, 400th and 4000th submission attempt (global ordinals). */
  static const u64 fail_at[] = { 4, 399, 3999 };
  u32 i;

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);
  rc_obs_init(&o, ref.input, RC_LEN);
  o.fail_at = fail_at;
  o.fail_cnt = sizeof(fail_at) / sizeof(fail_at[0]);

  s = bitflip_session_open(ref.input, RC_LEN, ref.base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &o);
  TASK_CHECK(s != NULL);

  /* Drive in chunks. Each injected failure surfaces as BFS_ERROR without
     billing or advancing; the next call retries the same candidate because
     the failed ordinals are consumed and never match again. */

  while (bitflip_session_status(s) != BFS_COMPLETE) {

    int rc = bitflip_session_run(s, 300);

    if (rc == BFS_ERROR) {
      TASK_CHECK(bitflip_session_status(s) == BFS_ERROR);
      continue;
    }

    TASK_CHECK(rc == BFS_PAUSED || rc == BFS_COMPLETE);

  }

  TASK_CHECK(o.fail_delivered == 3);
  TASK_CHECK(o.count == ref.obs.count);

  for (i = 0; i < o.count; i++) {
    TASK_CHECK(o.log[i].stage == ref.obs.log[i].stage);
    TASK_CHECK(o.log[i].pos == ref.obs.log[i].pos);
    TASK_CHECK(!memcmp(o.log[i].cand, ref.obs.log[i].cand, RC_LEN));
  }

  eff_len = 0;
  memcpy(eff, bitflip_session_eff_map(s, &eff_len), eff_len);
  TASK_CHECK(rc_compare(&ref, &o, eff, eff_len) == 0);

  bitflip_session_close(s);
  return 0;

}

static int case_interleaved_sessions(void) {

  static struct rc_reference ref_a;
  u8 input_b[140];
  u8 eff_a[RC_LEN / 8 + 1], eff_b[140 / 8 + 1];
  u32 eff_len;
  static struct rc_obs oa, ob;
  struct bitflip_session *a, *b;
  u64 step;
  int ra, rb;

  TASK_CHECK(rc_build_reference(&ref_a, 0) == 0);
  rc_make_input(input_b, sizeof(input_b), 71);

  rc_obs_init(&oa, ref_a.input, RC_LEN);
  rc_obs_init(&ob, input_b, sizeof(input_b));

  a = bitflip_session_open(ref_a.input, RC_LEN, ref_a.base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &oa);
  b = bitflip_session_open(input_b, sizeof(input_b), 0, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &ob);
  TASK_CHECK(a != NULL && b != NULL);

  /* Distinct work buffers: mutating one session never changes the other's
     input, and the sessions own their inputs. */

  TASK_CHECK(bitflip_session_input(a) != bitflip_session_input(b));
  TASK_CHECK(bitflip_session_len(b) == sizeof(input_b));
  TASK_CHECK(!memcmp(bitflip_session_input(b), input_b, sizeof(input_b)));

  for (step = 0; ; step++) {

    ra = bitflip_session_run(a, 17);
    rb = bitflip_session_run(b, 11);

    if (ra == BFS_COMPLETE && rb == BFS_COMPLETE) break;
    TASK_CHECK(step < 100000);

  }

  TASK_CHECK(oa.token_cnt == ref_a.obs.token_cnt);

  /* Session B keeps its own token stream and buffers, independent of A. */

  TASK_CHECK(ob.count > 0);
  TASK_CHECK(oa.count == ref_a.obs.count);

  eff_len = 0;
  memcpy(eff_a, bitflip_session_eff_map(a, &eff_len), eff_len);
  TASK_CHECK(rc_compare(&ref_a, &oa, eff_a, eff_len) == 0);

  /* Session B stands alone: its counts never borrowed A's buffers. */

  eff_len = 0;
  memcpy(eff_b, bitflip_session_eff_map(b, &eff_len), eff_len);
  TASK_CHECK(eff_len == RC_EFF_ALEN(sizeof(input_b)));
  TASK_CHECK(ob.count > 0);
  TASK_CHECK(bitflip_session_execs(b) == ob.count);

  bitflip_session_close(a);
  bitflip_session_close(b);
  return 0;

}

static int case_short_input_rules(void) {

  static const u32 lens[] = { 1, 2, 3, 4, 7 };
  static const u64 expect[][BFS_STAGES] = {
    /* len 1: */ { 8, 7, 5, 1, 0, 0 },
    /* len 2: */ { 16, 15, 13, 2, 1, 0 },
    /* len 3: */ { 24, 23, 21, 3, 2, 0 },
    /* len 4: */ { 32, 31, 29, 4, 3, 1 },
    /* len 7: */ { 56, 55, 53, 7, 6, 4 }
  };
  u32 k;

  for (k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {

    u8 in[7];
    static struct rc_obs o;
    struct bitflip_session *s;
    int st;

    rc_make_input(in, lens[k], (u8)(3 + k));
    rc_obs_init(&o, in, lens[k]);

    s = bitflip_session_open(in, lens[k], 0, 0, rc_exec_cb, rc_token_cb,
                             NULL, &o);
    TASK_CHECK(s != NULL);
    TASK_CHECK(bitflip_session_run(s, (u64)-1) == BFS_COMPLETE);

    for (st = 0; st < BFS_STAGES; st++)
      TASK_CHECK(bitflip_session_stage_execs(s, st) == expect[k][st]);

    TASK_CHECK(bitflip_session_execs(s) == o.count);
    bitflip_session_close(s);

  }

  return 0;

}

/* ------------------------------------------------------------------ */
/* checkpoint_handoff cases                                           */
/* ------------------------------------------------------------------ */

static const char *rc_ckpt_path(void) {
  return ".task-build/rc-checkpoint.bin";
}

/* Save after pausing at each of several stages, close, reload and finish;
   the concatenated sequence must equal the one-shot reference. */

static int case_checkpoint_resume_bounds(void) {

  static struct rc_reference ref;
  static const u64 pause_after[] = { 3, 50, 1280, 1300, 2800, 4000, 5000 };
  u32 k;

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);

  for (k = 0; k < sizeof(pause_after) / sizeof(pause_after[0]); k++) {

    static struct rc_obs before, after;
    struct bitflip_session *s;
    u8 eff[RC_LEN / 8 + 1];
    u32 eff_len, i;

    /* Phase 1: run up to the checkpoint point. */

    rc_obs_init(&before, ref.input, RC_LEN);
    s = bitflip_session_open(ref.input, RC_LEN, ref.base_cksum, 0,
                             rc_exec_cb, rc_token_cb, rc_event_cb, &before);
    TASK_CHECK(s != NULL);
    TASK_CHECK(bitflip_session_run(s, pause_after[k]) == BFS_PAUSED);
    TASK_CHECK(before.count == pause_after[k]);
    TASK_CHECK(bitflip_session_save(s, rc_ckpt_path()) == 0);
    bitflip_session_close(s);

    /* Phase 2: restore from the file and finish. */

    rc_obs_init(&after, ref.input, RC_LEN);
    s = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                             ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                             rc_event_cb, &after);
    TASK_CHECK(s != NULL);
    TASK_CHECK(bitflip_session_status(s) == BFS_PAUSED);
    TASK_CHECK(bitflip_session_execs(s) == before.count);
    TASK_CHECK(bitflip_session_run(s, (u64)-1) == BFS_COMPLETE);

    /* No candidate is replayed or skipped across the handoff. */

    TASK_CHECK(before.count + after.count == ref.obs.count);
    for (i = 0; i < before.count; i++) {
      TASK_CHECK(before.log[i].stage == ref.obs.log[i].stage);
      TASK_CHECK(before.log[i].pos == ref.obs.log[i].pos);
    }
    for (i = 0; i < after.count; i++) {
      TASK_CHECK(after.log[i].stage == ref.obs.log[before.count + i].stage);
      TASK_CHECK(after.log[i].pos == ref.obs.log[before.count + i].pos);
    }

    /* Tokens are delivered once, at completion, in the original order. */

    TASK_CHECK(after.token_cnt == ref.obs.token_cnt);
    TASK_CHECK(before.token_cnt == 0);
    for (i = 0; i < after.token_cnt; i++) {
      TASK_CHECK(after.tokens[i].len == ref.obs.tokens[i].len);
      TASK_CHECK(!memcmp(after.tokens[i].data, ref.obs.tokens[i].data,
                         after.tokens[i].len));
    }

    eff_len = 0;
    memcpy(eff, bitflip_session_eff_map(s, &eff_len), eff_len);
    TASK_CHECK(!memcmp(eff, ref.eff, eff_len));

    bitflip_session_close(s);
    unlink(rc_ckpt_path());

  }

  return 0;

}

/* Payload handed to the checkpoint-writing child process. */

struct rc_child_arg {
  u8 input[RC_LEN];
  u32 base_cksum;
  u64 budget;
  const char *path;
};

static int rc_child_main(struct rc_child_arg *arg) {

  static struct rc_obs o;
  struct bitflip_session *s;

  rc_obs_init(&o, arg->input, RC_LEN);
  s = bitflip_session_open(arg->input, RC_LEN, arg->base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &o);
  if (!s) _exit(2);
  if (bitflip_session_run(s, arg->budget) != BFS_PAUSED) _exit(3);
  if (bitflip_session_save(s, arg->path)) _exit(4);
  bitflip_session_close(s);
  _exit(0);

}

static int case_cross_process_handoff(void) {

  static struct rc_reference ref;
  struct rc_child_arg arg;
  static struct rc_obs after;
  struct bitflip_session *s;
  pid_t pid;
  int status;
  u8 eff[RC_LEN / 8 + 1];
  u32 eff_len, i;

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);

  memcpy(arg.input, ref.input, RC_LEN);
  arg.base_cksum = ref.base_cksum;
  arg.budget = 2345;
  arg.path = rc_ckpt_path();
  unlink(arg.path);

  pid = fork();
  TASK_CHECK(pid >= 0);

  if (pid == 0) rc_child_main(&arg);

  TASK_CHECK(waitpid(pid, &status, 0) == pid);
  TASK_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  /* Parent never held the session: pure file-based handoff. */

  rc_obs_init(&after, ref.input, RC_LEN);
  s = bitflip_session_load(arg.path, ref.input, RC_LEN, ref.base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &after);
  TASK_CHECK(s != NULL);
  TASK_CHECK(bitflip_session_execs(s) == arg.budget);
  TASK_CHECK(bitflip_session_run(s, (u64)-1) == BFS_COMPLETE);
  TASK_CHECK(after.count + arg.budget == ref.obs.count);

  for (i = 0; i < after.count; i++) {
    TASK_CHECK(after.log[i].stage ==
               ref.obs.log[(u32)arg.budget + i].stage);
    TASK_CHECK(after.log[i].pos ==
               ref.obs.log[(u32)arg.budget + i].pos);
  }

  TASK_CHECK(after.token_cnt == ref.obs.token_cnt);
  eff_len = 0;
  memcpy(eff, bitflip_session_eff_map(s, &eff_len), eff_len);
  TASK_CHECK(!memcmp(eff, ref.eff, eff_len));

  bitflip_session_close(s);
  unlink(arg.path);
  return 0;

}

static int rc_read_file(const char *path, u8 *buf, size_t cap, size_t *len) {

  int fd = open(path, O_RDONLY);
  ssize_t n;

  if (fd < 0) return -1;
  n = read(fd, buf, cap);
  close(fd);
  if (n <= 0) return -1;
  *len = (size_t)n;
  return 0;

}

static int rc_write_file(const char *path, const u8 *buf, size_t len) {

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

  if (fd < 0) return -1;
  if (write(fd, buf, len) != (ssize_t)len) { close(fd); return -1; }
  close(fd);
  return 0;

}

static int case_checkpoint_rejects_bad_images(void) {

  static struct rc_reference ref;
  static struct rc_obs o, sentinel_obs;
  struct bitflip_session *s, *victim, *loaded;
  u8 blob[65536];
  size_t len;
  u8 wrong_input[RC_LEN];
  u8 eff[RC_LEN / 8 + 1];
  u32 eff_len;

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);

  /* Build a legitimate checkpoint. */

  rc_obs_init(&o, ref.input, RC_LEN);
  s = bitflip_session_open(ref.input, RC_LEN, ref.base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &o);
  TASK_CHECK(s != NULL);
  TASK_CHECK(bitflip_session_run(s, 777) == BFS_PAUSED);
  TASK_CHECK(bitflip_session_save(s, rc_ckpt_path()) == 0);
  bitflip_session_close(s);
  TASK_CHECK(rc_read_file(rc_ckpt_path(), blob, sizeof(blob), &len) == 0);

  /* A live, unrelated session must remain usable after every failed load. */

  rc_obs_init(&sentinel_obs, ref.input, RC_LEN);
  victim = bitflip_session_open(ref.input, RC_LEN, ref.base_cksum, 0,
                                rc_exec_cb, rc_token_cb, rc_event_cb,
                                &sentinel_obs);
  TASK_CHECK(victim != NULL);

  rc_make_input(wrong_input, RC_LEN, 99);

  /* 1. Wrong input bytes: identity mismatch. */

  loaded = bitflip_session_load(rc_ckpt_path(), wrong_input, RC_LEN,
                                ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);

  /* 2. Wrong base checksum. */

  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum ^ 0xdeadbeef, 0, rc_exec_cb,
                                rc_token_cb, rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);

  /* 3. Wrong dumb_mode option. */

  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum, 1, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);

  /* 4. Truncated record. */

  TASK_CHECK(rc_write_file(rc_ckpt_path(), blob, len / 2) == 0);
  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);

  /* 5. Corrupted payload: CRC mismatch. */

  blob[len / 2] ^= 0x01;
  TASK_CHECK(rc_write_file(rc_ckpt_path(), blob, len) == 0);
  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);
  blob[len / 2] ^= 0x01;

  /* 6. Bad magic/version. */

  blob[0] ^= 0xff;
  TASK_CHECK(rc_write_file(rc_ckpt_path(), blob, len) == 0);
  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);
  blob[0] ^= 0xff;

  /* 7. Trailing garbage after an otherwise valid image. */

  {
    u8 scratch = 0x2a;
    int fd = open(rc_ckpt_path(), O_WRONLY | O_APPEND);
    TASK_CHECK(fd >= 0);
    TASK_CHECK(write(fd, &scratch, 1) == 1);
    close(fd);
  }
  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded == NULL);

  /* None of the rejected loads fired callbacks or submitted tokens. */

  TASK_CHECK(o.token_calls == 0);
  TASK_CHECK(o.event_calls > 0);       /* the original open's stage event   */

  /* The untouched sentinel session still runs to completion correctly. */

  TASK_CHECK(bitflip_session_run(victim, (u64)-1) == BFS_COMPLETE);
  eff_len = 0;
  memcpy(eff, bitflip_session_eff_map(victim, &eff_len), eff_len);
  TASK_CHECK(!memcmp(eff, ref.eff, eff_len));
  TASK_CHECK(sentinel_obs.count == ref.obs.count);
  bitflip_session_close(victim);

  /* 8. The pristine image still loads after all tampering. */

  TASK_CHECK(rc_write_file(rc_ckpt_path(), blob, len) == 0);
  loaded = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                                ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                                rc_event_cb, &o);
  TASK_CHECK(loaded != NULL);
  bitflip_session_close(loaded);

  unlink(rc_ckpt_path());
  return 0;

}

static int case_completed_session_no_replay(void) {

  static struct rc_reference ref;
  static struct rc_obs o;
  struct bitflip_session *s;

  TASK_CHECK(rc_build_reference(&ref, 0) == 0);

  rc_obs_init(&o, ref.input, RC_LEN);
  s = bitflip_session_open(ref.input, RC_LEN, ref.base_cksum, 0,
                           rc_exec_cb, rc_token_cb, rc_event_cb, &o);
  TASK_CHECK(s != NULL);
  TASK_CHECK(bitflip_session_run(s, (u64)-1) == BFS_COMPLETE);
  TASK_CHECK(bitflip_session_save(s, rc_ckpt_path()) == 0);
  bitflip_session_close(s);

  /* Reloading a finished session must not re-run candidates or re-deliver
     the token proposals. */

  rc_obs_init(&o, ref.input, RC_LEN);
  s = bitflip_session_load(rc_ckpt_path(), ref.input, RC_LEN,
                           ref.base_cksum, 0, rc_exec_cb, rc_token_cb,
                           rc_event_cb, &o);
  TASK_CHECK(s != NULL);
  TASK_CHECK(bitflip_session_status(s) == BFS_COMPLETE);
  TASK_CHECK(bitflip_session_run(s, (u64)-1) == BFS_COMPLETE);
  TASK_CHECK(o.count == 0);
  TASK_CHECK(o.token_calls == 0);
  TASK_CHECK(bitflip_session_execs(s) == ref.obs.count);

  bitflip_session_close(s);
  unlink(rc_ckpt_path());
  return 0;

}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

static int case_production_path(void) {

  return production_path_run();

}

static int case_production_full_run(void) {

  return production_full_run();

}

const struct task_case task_cases[] = {

  { "slice_feedback", "full_vs_sliced",          case_full_vs_sliced },
  { "slice_feedback", "alternate_budget_schedule", case_alternate_budget_schedule },
  { "slice_feedback", "zero_budget_is_free",     case_zero_budget_is_free },
  { "slice_feedback", "failed_candidate_retries", case_failed_candidate_retries },
  { "slice_feedback", "interleaved_sessions",    case_interleaved_sessions },
  { "slice_feedback", "short_input_rules",       case_short_input_rules },
  { "slice_feedback", "production_fuzz_one_path", case_production_path },
  { "slice_feedback", "production_fuzz_one_unbounded", case_production_full_run },

  { "checkpoint_handoff", "resume_across_stage_bounds",
    case_checkpoint_resume_bounds },
  { "checkpoint_handoff", "cross_process_handoff",
    case_cross_process_handoff },
  { "checkpoint_handoff", "rejects_corrupt_or_foreign_images",
    case_checkpoint_rejects_bad_images },
  { "checkpoint_handoff", "completed_session_does_not_replay",
    case_completed_session_no_replay }

};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
