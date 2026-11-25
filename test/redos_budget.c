#include "redos_model.h"
#include "../checks/budget_check.h"

#include <oniguruma.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define RB_POS_CAP 64

typedef struct {
  const UChar *base;
  int pos[RB_POS_CAP];
  int n;
  int overflow;
} Recorder;

typedef struct {
  int pos[RB_POS_CAP];
  int n;
  int overflow;
  int rc;
} SearchRun;

typedef struct {
  int probed;
  int cost_ok;
  int model_match;
  rb_reference ref;
  int unlimited_rc;
  unsigned long retry;
  unsigned long calls;
} Cost;

typedef struct {
  int rc;
  int pos;
  int index;
  unsigned long match_limit;
  unsigned long search_limit;
  unsigned long call_limit;
} Pred;

typedef struct {
  int ready;
  int compile_rc;
  rb_reference ref;

  int seq_n[2];
  int seq[2][RB_MAX_POSITIONS];
  int unlimited_rc[2];

  Cost cost[RB_MAX_POSITIONS];
  unsigned long max_retry;
  unsigned long max_calls;

  unsigned long match_limit;
  unsigned long search_limit;
  unsigned long call_limit;
  Pred retry_pred;
  Pred retry_actual;
  Pred call_pred;
  Pred call_actual;
  int retry_diff;
  int call_diff;
  char first_diff[512];
} Signature;

typedef struct {
  int active;
  int replay;
  int reported;
  char dir[4096];
  rb_spec spec;
  int has_sig;
  Signature sig;
  int reduced_ok;
  int reduction_attempts;
  char original_path[4200];
  char reduced_path[4200];
  char replay_path[4200];
} FailureHub;

static FailureHub g_hub;
static int g_onig_ready = 0;

static void rb_onig_init(void) {
  if (!g_onig_ready) {
    OnigEncoding enc = ONIG_ENCODING_ASCII;
    int r = onig_initialize(&enc, 1);
    if (r == 0) g_onig_ready = 1;
  }
}

static const char *rb_rc_name(int rc) {
  static char buf[64];
  switch (rc) {
    case ONIG_MISMATCH: return "ONIG_MISMATCH";
    case ONIGERR_RETRY_LIMIT_IN_MATCH_OVER:
      return "ONIGERR_RETRY_LIMIT_IN_MATCH_OVER";
    case ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER:
      return "ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER";
    case ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER:
      return "ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER";
    default:
      snprintf(buf, sizeof(buf), "CODE_%d", rc);
      return buf;
  }
}

static int rb_is_retry_error(int rc) {
  return rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER ||
         rc == ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER;
}

static int record_callout(OnigCalloutArgs *args, void *user_data) {
  Recorder *r = (Recorder *)user_data;
  OnigCalloutIn in = onig_get_callout_in_by_callout_args(args);
  if (r && in == ONIG_CALLOUT_IN_PROGRESS) {
    const UChar *cur = onig_get_current_by_callout_args(args);
    int pos = (int)(cur - r->base);
    if (r->n < RB_POS_CAP) r->pos[r->n++] = pos;
    else r->overflow = 1;
  }
  return ONIG_CALLOUT_SUCCESS;
}

static OnigMatchParam *rb_make_mp(unsigned long match_limit,
                                  unsigned long search_limit,
                                  Recorder *rec) {
  OnigMatchParam *mp = onig_new_match_param();
  if (!mp) return 0;
  onig_set_retry_limit_in_match_of_match_param(mp, match_limit);
  onig_set_retry_limit_in_search_of_match_param(mp, search_limit);
  if (rec) {
    onig_set_progress_callout_of_match_param(mp, record_callout);
    onig_set_callout_user_data_of_match_param(mp, rec);
  }
  return mp;
}

static int rb_compile(const rb_spec *spec, OnigRegex *reg) {
  char pat[RB_MAX_PAT + 16];
  int n = rb_build_pattern(spec, 1, pat, sizeof(pat));
  OnigErrorInfo ei;
  if (n < 0) return ONIGERR_INVALID_ARGUMENT;
  return onig_new(reg, (const OnigUChar *)pat,
                  (const OnigUChar *)(pat + n), 0,
                  ONIG_ENCODING_ASCII, ONIG_SYNTAX_ONIGURUMA, &ei);
}

static int run_match(OnigRegex reg, const rb_spec *spec, int pos,
                     unsigned long match_limit,
                     unsigned long search_limit,
                     Recorder *rec, OnigRegion *region) {
  OnigMatchParam *mp;
  OnigRegion *local_region = 0;
  int rc;
  if (rec) {
    rec->base = (const UChar *)spec->text;
    rec->n = 0;
    rec->overflow = 0;
  }
  mp = rb_make_mp(match_limit, search_limit, rec);
  if (!mp) return ONIGERR_MEMORY;
  if (region) local_region = region;
  else local_region = onig_region_new();
  rc = onig_match_with_param(
      reg, (const UChar *)spec->text,
      (const UChar *)(spec->text + spec->text_len),
      (const UChar *)(spec->text + pos), local_region, 0, mp);
  onig_free_match_param(mp);
  if (!region) onig_region_free(local_region, 1);
  return rc;
}

static SearchRun run_search(OnigRegex reg, const rb_spec *spec, int direction,
                            unsigned long match_limit,
                            unsigned long search_limit) {
  SearchRun out;
  Recorder rec;
  OnigMatchParam *mp;
  OnigRegion *region;
  const UChar *str = (const UChar *)spec->text;
  const UChar *end = str + spec->text_len;

  memset(&out, 0, sizeof(out));
  memset(&rec, 0, sizeof(rec));
  rec.base = str;
  mp = rb_make_mp(match_limit, search_limit, &rec);
  region = onig_region_new();
  if (!mp || !region) {
    out.rc = ONIGERR_MEMORY;
  } else if (direction == RB_FORWARD) {
    out.rc = onig_search_with_param(reg, str, end, str, end, region, 0, mp);
  } else {
    /* start=end and range=str requests Oniguruma's backward loop. */
    out.rc = onig_search_with_param(reg, str, end, end, str, region, 0, mp);
  }
  if (mp) onig_free_match_param(mp);
  if (region) onig_region_free(region, 1);

  out.n = rec.n < RB_POS_CAP ? rec.n : RB_POS_CAP;
  for (int i = 0; i < out.n; i++) out.pos[i] = rec.pos[i];
  out.overflow = rec.overflow;
  return out;
}

static unsigned long probe_retry(OnigRegex reg, const rb_spec *spec, int pos,
                                 unsigned long upper, int *ok) {
  int rc;
  unsigned long lo = 1, hi = upper;
  *ok = 1;

  rc = run_match(reg, spec, pos, hi, 0, 0, 0);
  if (rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER) {
    *ok = 0;
    return upper;
  }
  while (lo < hi) {
    unsigned long mid = lo + (hi - lo) / 2;
    rc = run_match(reg, spec, pos, mid, 0, 0, 0);
    if (rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER) lo = mid + 1;
    else hi = mid;
  }
  return lo - 1;
}

static unsigned long probe_calls(OnigRegex reg, const rb_spec *spec, int pos,
                                 unsigned long upper, int *ok) {
  unsigned long old = onig_get_subexp_call_limit_in_search();
  unsigned long lo = 1, hi = upper;
  int rc;
  *ok = 1;

  onig_set_subexp_call_limit_in_search(hi);
  rc = run_match(reg, spec, pos, 0, 0, 0, 0);
  onig_set_subexp_call_limit_in_search(old);
  if (rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER) {
    *ok = 0;
    return upper;
  }

  while (lo < hi) {
    unsigned long mid = lo + (hi - lo) / 2;
    onig_set_subexp_call_limit_in_search(mid);
    rc = run_match(reg, spec, pos, 0, 0, 0, 0);
    if (rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER) lo = mid + 1;
    else hi = mid;
  }
  onig_set_subexp_call_limit_in_search(old);
  return lo;
}

static void profile_spec(OnigRegex reg, const rb_spec *spec, Cost *cost) {
  unsigned long upper = (unsigned long)spec->expansion + 4;
  if (upper < 64) upper = 64;
  if (upper > RB_BRANCH_ATTEMPT_LIMIT + 8)
    upper = RB_BRANCH_ATTEMPT_LIMIT + 8;

  for (int pos = 0; pos <= spec->text_len; pos++) {
    Cost *c = &cost[pos];
    int ok_retry = 1, ok_calls = 1;
    memset(c, 0, sizeof(*c));
    c->probed = 1;
    rb_match_reference(spec, pos, &c->ref);
    c->model_match = c->ref.match;
    c->unlimited_rc = run_match(reg, spec, pos, 0, 0, 0, 0);
    c->retry = probe_retry(reg, spec, pos, upper, &ok_retry);
    if (spec->kind == RB_KIND_BRANCH) {
      c->calls = 0;
    } else {
      c->calls = probe_calls(reg, spec, pos, upper, &ok_calls);
    }
    c->cost_ok = ok_retry && ok_calls;
  }
}

static Pred predict_retry(const int *seq, int n, const Cost *cost,
                          unsigned long match_limit,
                          unsigned long search_limit) {
  Pred p;
  unsigned long used = 0;
  memset(&p, 0, sizeof(p));
  p.rc = ONIG_MISMATCH;
  p.pos = -1;
  p.index = -1;
  p.match_limit = match_limit;
  p.search_limit = search_limit;

  for (int i = 0; i < n; i++) {
    int pos = seq[i];
    unsigned long r = cost[pos].retry;
    unsigned long effective = match_limit;
    if (search_limit != 0) {
      unsigned long rem = search_limit - used;
      if (match_limit == 0 || rem < effective) effective = rem;
    }
    if (effective != 0 && r >= effective) {
      p.index = i;
      p.pos = pos;
      if (match_limit != 0 && effective >= match_limit)
        p.rc = ONIGERR_RETRY_LIMIT_IN_MATCH_OVER;
      else
        p.rc = ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER;
      return p;
    }
    used += r;
  }
  return p;
}

static Pred predict_calls(const int *seq, int n, const Cost *cost,
                          unsigned long call_limit) {
  Pred p;
  unsigned long used = 0;
  memset(&p, 0, sizeof(p));
  p.rc = ONIG_MISMATCH;
  p.pos = -1;
  p.index = -1;
  p.call_limit = call_limit;

  for (int i = 0; i < n; i++) {
    int pos = seq[i];
    unsigned long c = cost[pos].calls;
    if (call_limit != 0 && used + c > call_limit) {
      p.rc = ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER;
      p.pos = pos;
      p.index = i;
      return p;
    }
    used += c;
  }
  return p;
}

static Pred actual_pred(const SearchRun *run) {
  Pred p;
  memset(&p, 0, sizeof(p));
  p.rc = run->rc;
  p.pos = -1;
  p.index = -1;
  if (run->rc >= 0) p.pos = run->rc;
  else if ((rb_is_retry_error(run->rc) ||
            run->rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER) &&
           run->n > 0)
    p.pos = run->pos[run->n - 1];
  return p;
}

static void copy_seq(int *dst, int *dst_n, const SearchRun *run) {
  *dst_n = run->n;
  for (int i = 0; i < run->n && i < RB_MAX_POSITIONS; i++)
    dst[i] = run->pos[i];
}

static Signature evaluate_signature(const rb_spec *spec) {
  Signature sig;
  OnigRegex reg = 0;
  SearchRun run;
  unsigned long old_calls = onig_get_subexp_call_limit_in_search();

  memset(&sig, 0, sizeof(sig));
  sig.ready = 1;
  sig.compile_rc = rb_compile(spec, &reg);
  if (sig.compile_rc != 0) return sig;

  sig.ref = rb_reference_search(spec);
  profile_spec(reg, spec, sig.cost);

  run = run_search(reg, spec, RB_FORWARD, 0, 0);
  sig.unlimited_rc[RB_FORWARD] = run.rc;
  copy_seq(sig.seq[RB_FORWARD], &sig.seq_n[RB_FORWARD], &run);

  run = run_search(reg, spec, RB_BACKWARD, 0, 0);
  sig.unlimited_rc[RB_BACKWARD] = run.rc;
  copy_seq(sig.seq[RB_BACKWARD], &sig.seq_n[RB_BACKWARD], &run);

  for (int i = 0; i < sig.seq_n[RB_FORWARD]; i++) {
    int pos = sig.seq[RB_FORWARD][i];
    if (sig.cost[pos].retry > sig.max_retry)
      sig.max_retry = sig.cost[pos].retry;
    if (sig.cost[pos].calls > sig.max_calls)
      sig.max_calls = sig.cost[pos].calls;
  }

  if (sig.seq_n[RB_FORWARD] >= 2) {
    int p0 = sig.seq[RB_FORWARD][0];
    int p1 = sig.seq[RB_FORWARD][1];
    unsigned long r0 = sig.cost[p0].retry;
    unsigned long r1 = sig.cost[p1].retry;
    unsigned long rem = r1 >= 2 ? r1 / 2 : 1;
    SearchRun actual;

    sig.match_limit = sig.max_retry + 1;
    sig.search_limit = r0 + rem;
    sig.retry_pred = predict_retry(sig.seq[RB_FORWARD],
                                   sig.seq_n[RB_FORWARD], sig.cost,
                                   sig.match_limit, sig.search_limit);
    sig.retry_pred.match_limit = sig.match_limit;
    sig.retry_pred.search_limit = sig.search_limit;

    actual = run_search(reg, spec, RB_FORWARD, sig.match_limit,
                        sig.search_limit);
    sig.retry_actual = actual_pred(&actual);
    sig.retry_actual.match_limit = sig.match_limit;
    sig.retry_actual.search_limit = sig.search_limit;
    sig.retry_diff = sig.retry_pred.rc != sig.retry_actual.rc ||
                     sig.retry_pred.pos != sig.retry_actual.pos;
    if (sig.retry_diff) {
      snprintf(sig.first_diff, sizeof(sig.first_diff),
               "retry expected=%d/%s@%d actual=%d/%s@%d limits M=%lu S=%lu",
               sig.retry_pred.rc, rb_rc_name(sig.retry_pred.rc),
               sig.retry_pred.pos, sig.retry_actual.rc,
               rb_rc_name(sig.retry_actual.rc), sig.retry_actual.pos,
               sig.match_limit, sig.search_limit);
    }
  }

  if (sig.max_calls > 0 && sig.seq_n[RB_FORWARD] >= 2) {
    SearchRun actual;
    sig.call_limit = sig.max_calls;
    sig.call_pred = predict_calls(sig.seq[RB_FORWARD],
                                  sig.seq_n[RB_FORWARD], sig.cost,
                                  sig.call_limit);
    onig_set_subexp_call_limit_in_search(sig.call_limit);
    actual = run_search(reg, spec, RB_FORWARD, 0, 0);
    onig_set_subexp_call_limit_in_search(old_calls);
    sig.call_actual = actual_pred(&actual);
    sig.call_actual.call_limit = sig.call_limit;
    sig.call_diff = sig.call_pred.rc != sig.call_actual.rc ||
                    sig.call_pred.pos != sig.call_actual.pos;
    if (sig.call_diff && !sig.first_diff[0]) {
      snprintf(sig.first_diff, sizeof(sig.first_diff),
               "calls expected=%d/%s@%d actual=%d/%s@%d G=%lu",
               sig.call_pred.rc, rb_rc_name(sig.call_pred.rc),
               sig.call_pred.pos, sig.call_actual.rc,
               rb_rc_name(sig.call_actual.rc), sig.call_actual.pos,
               sig.call_limit);
    }
  }

  onig_set_subexp_call_limit_in_search(old_calls);
  onig_free(reg);
  return sig;
}

static void ensure_artifact_dir(void) {
  const char *env = getenv("REDOS_ARTIFACT_DIR");
  char cwd[4096];
  if (g_hub.dir[0]) return;
  if (env && *env) {
    if (env[0] == '/') snprintf(g_hub.dir, sizeof(g_hub.dir), "%s", env);
    else {
      if (getcwd(cwd, sizeof(cwd)))
        snprintf(g_hub.dir, sizeof(g_hub.dir), "%s/%s", cwd, env);
      else
        snprintf(g_hub.dir, sizeof(g_hub.dir), "%s", env);
    }
  } else if (getcwd(cwd, sizeof(cwd))) {
    snprintf(g_hub.dir, sizeof(g_hub.dir), "%s/.budget-failures", cwd);
  } else {
    snprintf(g_hub.dir, sizeof(g_hub.dir), ".budget-failures");
  }
  mkdir(g_hub.dir, 0755);
}

static int reduction_preserves(const rb_spec *orig, const Signature *os,
                               const rb_spec *cand, const Signature *cs) {
  if (!cs->ready || cs->compile_rc != 0) return 0;
  if (cs->ref.match) return 0;
  if (cs->seq_n[RB_FORWARD] < 2) return 0;

  if (orig->plan == RB_PLAN_CALL_TOTAL) {
    if (os->call_pred.rc != ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER) return 0;
    if (cs->call_pred.rc != os->call_pred.rc) return 0;
    if (cs->call_pred.index < 1) return 0;
    if (!cs->call_diff) return 0;
    if (cs->call_actual.rc != os->call_actual.rc) return 0;
    return 1;
  }

  if (os->retry_pred.rc != ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER) return 0;
  if (cs->retry_pred.rc != os->retry_pred.rc) return 0;
  if (cs->retry_pred.index < 1) return 0;
  if (!cs->retry_diff) return 0;
  if (cs->retry_actual.rc != os->retry_actual.rc) return 0;
  return 1;
}

static void write_text_artifact(const char *path, const rb_spec *spec,
                                const Signature *sig, const char *extra) {
  FILE *f = fopen(path, "w");
  char observed_pat[RB_MAX_PAT + 16];
  int observed_len = rb_build_pattern(spec, 1, observed_pat,
                                      sizeof(observed_pat));
  if (!f) return;
  fprintf(f, "group=%s seed=%u case=%d\n", spec->group, spec->seed,
          spec->case_id);
  fprintf(f, "canonical_direction=forward tested_directions=forward,backward "
             "spec_direction=%s\n",
          rb_direction_name(spec->direction));
  fprintf(f, "kind=%s (%d)\n", rb_kind_name(spec->kind), spec->kind);
  fprintf(f, "fragments ordered=\"%s\"|\"%s\" nf=%d swapped=%d\n",
          spec->frag[0], spec->frag[1], spec->nf, spec->order_swapped);
  fprintf(f, "repeat={%d,%d} na=%d suffix_byte=%d depth=%d\n", spec->lo,
          spec->hi, spec->na, (unsigned char)spec->suffix, spec->depth);
  fprintf(f, "pattern_len=%d pattern=\"%s\"\n", spec->pat_len, spec->pat);
  fprintf(f, "base_pattern_len=%d base_pattern=\"%s\"\n", spec->pat_len,
          spec->pat);
  fprintf(f, "observed_pattern_len=%d observed_pattern=\"%s\"\n",
          observed_len, observed_pat);
  fprintf(f, "text_len=%d text=\"%.*s\"\n", spec->text_len, spec->text_len,
          spec->text);
  fprintf(f, "expansion_bound=%llu limit=%d allowed=%d\n", spec->expansion,
          RB_BRANCH_ATTEMPT_LIMIT, spec->allowed);
  if (sig && sig->ready) {
    fprintf(f, "reference_match=%d compile_rc=%d\n", sig->ref.match,
            sig->compile_rc);
    for (int d = 0; d < 2; d++) {
      fprintf(f, "%s_unlimited_rc=%s(%d) starts=",
              rb_direction_name(d), rb_rc_name(sig->unlimited_rc[d]),
              sig->unlimited_rc[d]);
      for (int i = 0; i < sig->seq_n[d]; i++)
        fprintf(f, "%s%d", i ? "," : "", sig->seq[d][i]);
      fputc('\n', f);
    }
    fprintf(f, "per_start_costs pos model unlimited retry calls\n");
    for (int pos = 0; pos <= spec->text_len; pos++) {
      const Cost *c = &sig->cost[pos];
      if (!c->probed) continue;
      fprintf(f, "  %2d model=%d unlimited=%d retry=%lu calls=%lu ok=%d\n",
              pos, c->model_match, c->unlimited_rc, c->retry, c->calls,
              c->cost_ok);
    }
    fprintf(f, "limits M=%lu S=%lu G=%lu\n", sig->match_limit,
            sig->search_limit, sig->call_limit);
    fprintf(f, "retry expected=%s@%d actual=%s@%d diff=%d\n",
            rb_rc_name(sig->retry_pred.rc), sig->retry_pred.pos,
            rb_rc_name(sig->retry_actual.rc), sig->retry_actual.pos,
            sig->retry_diff);
    fprintf(f, "calls expected=%s@%d actual=%s@%d diff=%d\n",
            rb_rc_name(sig->call_pred.rc), sig->call_pred.pos,
            rb_rc_name(sig->call_actual.rc), sig->call_actual.pos,
            sig->call_diff);
    if (sig->first_diff[0]) fprintf(f, "first_diff=%s\n", sig->first_diff);
  }
  if (extra) fputs(extra, f);
  fclose(f);
}

static void run_reduction(void) {
  rb_spec cur, best;
  Signature best_sig;
  int accepted = 0;
  if (!g_hub.has_sig) {
    g_hub.sig = evaluate_signature(&g_hub.spec);
    g_hub.has_sig = 1;
  }
  best = cur = g_hub.spec;
  best_sig = g_hub.sig;

  for (int attempt = 0; attempt < RB_REDUCTION_ATTEMPTS; attempt++) {
    rb_spec cand = cur;
    Signature cs;
    int transform = attempt % 3;
    if (transform == 0) {
      if (cand.kind == RB_KIND_RECURSE || cand.kind == RB_KIND_RECALL) {
        if (cand.depth > 1) cand.depth--;
        else continue;
      } else {
        if (cand.na > 2) cand.na--;
        else continue;
      }
    } else if (transform == 1) {
      if (cand.hi > cand.lo) cand.hi--;
      else if (cand.lo > 1) { cand.lo--; cand.hi--; }
      else continue;
    } else {
      if (cand.nf == 2) cand.nf = 1;
      else continue;
    }
    rb_refresh(&cand);
    if (!rb_spec_within_limits(&cand)) continue;
    cs = evaluate_signature(&cand);
    g_hub.reduction_attempts++;
    if (reduction_preserves(&g_hub.spec, &g_hub.sig, &cand, &cs)) {
      cur = cand;
      best = cand;
      best_sig = cs;
      accepted++;
    }
  }

  ensure_artifact_dir();
  snprintf(g_hub.original_path, sizeof(g_hub.original_path),
           "%s/original.spec", g_hub.dir);
  snprintf(g_hub.reduced_path, sizeof(g_hub.reduced_path),
           "%s/reduced.spec", g_hub.dir);
  snprintf(g_hub.replay_path, sizeof(g_hub.replay_path),
           "%s/replay.sh", g_hub.dir);
  rb_write_spec(&g_hub.spec, g_hub.original_path);

  if (accepted > 0) {
    FILE *sh;
    char cwd[4096];
    char reduced_txt[4200];
    rb_write_spec(&best, g_hub.reduced_path);
    snprintf(reduced_txt, sizeof(reduced_txt), "%s/reduced.txt", g_hub.dir);
    g_hub.reduced_ok = 1;
    if (getcwd(cwd, sizeof(cwd))) {
      sh = fopen(g_hub.replay_path, "w");
      if (sh) {
        fprintf(sh, "#!/usr/bin/env bash\nset -euo pipefail\n");
        fprintf(sh, "ROOT='%s'\n", cwd);
        fprintf(sh, "SPEC='%s'\n", g_hub.reduced_path);
        fprintf(sh, "export REDOS_REPLAY_SPEC=\"$SPEC\"\n");
        fprintf(sh,
                "exec bash \"$ROOT/checks/run.sh\" target --group %s "
                "--seed %u --case %d\n",
                best.group, best.seed, best.case_id);
        fclose(sh);
        chmod(g_hub.replay_path, 0755);
      }
    }
    write_text_artifact(reduced_txt, &best, &best_sig,
                        "reduced=true\n");
    printf("REDUCTION_OK accepted=%d attempts=%d original_text=%d "
           "reduced_text=%d reduced_spec=%s replay=%s\n",
           accepted, g_hub.reduction_attempts, g_hub.spec.text_len,
           best.text_len, g_hub.reduced_path, g_hub.replay_path);
  } else {
    rb_write_spec(&g_hub.spec, g_hub.reduced_path);
    printf("REDUCTION_NOT_PRESERVED attempts=%d spec=%s\n",
           g_hub.reduction_attempts, g_hub.reduced_path);
  }
}

static void rb_check_failed(const char *expr, const char *file, int line) {
  if (g_hub.replay) {
    printf("REPLAY_CHECK_FAIL %s:%d (%s)\n", file, line, expr);
    if (g_hub.has_sig && g_hub.sig.first_diff[0])
      printf("REPLAY_FIRST_DIFF %s\n", g_hub.sig.first_diff);
    return;
  }
  if (g_hub.reported) return;
  g_hub.reported = 1;
  ensure_artifact_dir();
  if (!g_hub.has_sig) {
    g_hub.sig = evaluate_signature(&g_hub.spec);
    g_hub.has_sig = 1;
  }
  snprintf(g_hub.original_path, sizeof(g_hub.original_path),
           "%s/failure.txt", g_hub.dir);
  write_text_artifact(g_hub.original_path, &g_hub.spec, &g_hub.sig, expr);
  printf("BUDGET_FIRST_DIFF %s:%d: %s | %s\n", file, line, expr,
         g_hub.sig.first_diff[0] ? g_hub.sig.first_diff : "(see failure.txt)");
  printf("FAILURE_ARTIFACT_DIR=%s\n", g_hub.dir);
  run_reduction();
}

#define RB_CHECK(expr)                                                       \
  do {                                                                       \
    int rb_check_value_ = !!(expr);                                          \
    BUDGET_CHECK(rb_check_value_);                                           \
    if (!rb_check_value_)                                                    \
      rb_check_failed(#expr, __FILE__, __LINE__);                            \
  } while (0)

static void check_hand_examples(void) {
  struct example {
    const char *pat;
    const char *text;
    int rc;
    int num;
    int c0b, c0e, c1b, c1e, c2b, c2e;
  };
  static const struct example ex[] = {
      {"((a|aa)+)\\z", "aaaa", 0, 3, 0, 4, 0, 4, 3, 4},
      {"((aa|a)+)\\z", "aaaa", 0, 3, 0, 4, 0, 4, 2, 4},
      {"(?<r>a\\g<r>?b){0}\\g<r>\\z", "aaabbb", 0, 2, 0, 6, 0, 6, -1, -1},
      {"(?<item>a|aa){0}(?:\\g<item>)+\\z", "aaaa", 0, 2, 0, 4, 3, 4, -1, -1},
  };
  for (unsigned i = 0; i < sizeof(ex) / sizeof(ex[0]); i++) {
    OnigRegex reg;
    OnigErrorInfo ei;
    OnigRegion *region = onig_region_new();
    const UChar *p = (const UChar *)ex[i].pat;
    const UChar *s = (const UChar *)ex[i].text;
    int cr = onig_new(&reg, p, p + strlen(ex[i].pat), 0,
                      ONIG_ENCODING_ASCII, ONIG_SYNTAX_ONIGURUMA, &ei);
    RB_CHECK(cr == 0);
    if (cr != 0) {
      onig_region_free(region, 1);
      continue;
    }
    int rc = onig_search(reg, s, s + strlen(ex[i].text), s,
                         s + strlen(ex[i].text), region, 0);
    RB_CHECK(rc == ex[i].rc);
    RB_CHECK(region->num_regs == ex[i].num);
    RB_CHECK(region->beg[0] == ex[i].c0b && region->end[0] == ex[i].c0e);
    RB_CHECK(region->beg[1] == ex[i].c1b && region->end[1] == ex[i].c1e);
    if (ex[i].num == 3)
      RB_CHECK(region->beg[2] == ex[i].c2b && region->end[2] == ex[i].c2e);
    onig_region_free(region, 1);
    onig_free(reg);
  }
}

static void check_seed_tsv(void) {
  FILE *f = fopen("checks/budget_inputs.tsv", "r");
  char line[512];
  int rows = 0;
  if (!f) {
    RB_CHECK(0 && "checks/budget_inputs.tsv is readable from repository root");
    return;
  }
  while (fgets(line, sizeof(line), f)) {
    char *tab;
    OnigRegex reg;
    OnigErrorInfo ei;
    int cr;
    if (line[0] == '#' || line[0] == '\n') continue;
    tab = strchr(line, '\t');
    if (!tab) continue;
    *tab = 0;
    char *pat = line;
    char *text = tab + 1;
    text[strcspn(text, "\r\n")] = 0;
    rows++;
    cr = onig_new(&reg, (const UChar *)pat,
                  (const UChar *)(pat + strlen(pat)), 0,
                  ONIG_ENCODING_ASCII, ONIG_SYNTAX_ONIGURUMA, &ei);
    RB_CHECK(cr == 0);
    if (cr == 0) {
      OnigRegion *r = onig_region_new();
      const UChar *s = (const UChar *)text;
      int rc = onig_search(reg, s, s + strlen(text), s, s + strlen(text),
                           r, 0);
      RB_CHECK(rc == ONIG_MISMATCH || rc >= 0);
      onig_region_free(r, 1);
      onig_free(reg);
    }
  }
  fclose(f);
  RB_CHECK(rows == 4);
}

static void check_corpus_case(const rb_spec *spec) {
  Signature sig;
  RB_CHECK(spec->allowed);
  if (!spec->allowed) return;
  sig = evaluate_signature(spec);
  g_hub.sig = sig;
  g_hub.has_sig = 1;

  RB_CHECK(spec->text_len <= RB_MAX_TEXT);
  RB_CHECK(spec->depth <= 5);
  RB_CHECK(spec->expansion <= RB_BRANCH_ATTEMPT_LIMIT);
  RB_CHECK(sig.compile_rc == 0);
  if (sig.compile_rc != 0) return;

  RB_CHECK(sig.unlimited_rc[RB_FORWARD] ==
           (sig.ref.match ? sig.ref.pos : ONIG_MISMATCH));
  RB_CHECK(sig.seq_n[RB_FORWARD] >= 1 || sig.ref.match);

  for (int pos = 0; pos <= spec->text_len; pos++) {
    const Cost *c = &sig.cost[pos];
    RB_CHECK(c->probed && c->cost_ok);
    RB_CHECK((c->unlimited_rc >= 0) == c->model_match);
    if (c->model_match) {
      RB_CHECK(c->unlimited_rc == c->ref.end - pos);
      RB_CHECK(c->ref.num_regs >= 2);
    } else {
      RB_CHECK(c->unlimited_rc == ONIG_MISMATCH);
    }
  }

  if (spec->case_id == 0) {
    check_hand_examples();
    check_seed_tsv();
    rb_spec huge = *spec;
    huge.nf = 2;
    huge.hi = 20;
    rb_refresh(&huge);
    RB_CHECK(!rb_spec_within_limits(&huge));
  }
}

static void check_start_geometry(const rb_spec *spec, const Signature *sig) {
  int upper_last = spec->na - 1;
  int lower_last = 0;
  if (spec->kind == RB_KIND_BRANCH || spec->kind == RB_KIND_CALL) {
    int max_frag = spec->frag_len[0] > spec->frag_len[1]
                       ? spec->frag_len[0]
                       : spec->frag_len[1];
    int required_max = spec->lo * max_frag;
    lower_last = spec->na - required_max - 1;
    if (lower_last < 0) lower_last = 0;
  }

  RB_CHECK(!sig->ref.match);
  RB_CHECK(sig->unlimited_rc[RB_FORWARD] == ONIG_MISMATCH);
  RB_CHECK(sig->unlimited_rc[RB_BACKWARD] == ONIG_MISMATCH);

  RB_CHECK(sig->seq_n[RB_FORWARD] >= 2);
  RB_CHECK(sig->seq_n[RB_BACKWARD] >= 2);
  RB_CHECK(sig->seq_n[RB_FORWARD] == sig->seq_n[RB_BACKWARD]);
  {
    int first = sig->seq[RB_FORWARD][0];
    int last = sig->seq[RB_FORWARD][sig->seq_n[RB_FORWARD] - 1];
    for (int i = 0; i < sig->seq_n[RB_FORWARD]; i++) {
      int pos = sig->seq[RB_FORWARD][i];
      RB_CHECK(pos == first + i);
      RB_CHECK(pos >= 0 && pos <= upper_last);
    }
    for (int i = 0; i < sig->seq_n[RB_BACKWARD]; i++) {
      int pos = sig->seq[RB_BACKWARD][i];
      RB_CHECK(pos == last - i);
      RB_CHECK(pos >= first && pos <= last);
    }
    RB_CHECK(last >= lower_last && last <= upper_last);
    RB_CHECK(sig->seq[RB_BACKWARD][0] == last);
    RB_CHECK(sig->seq[RB_BACKWARD][sig->seq_n[RB_BACKWARD] - 1] == first);
  }
}

static void check_retry_budget_case(const rb_spec *spec,
                                    const Signature *sig) {
  OnigRegex reg;
  int cr = rb_compile(spec, &reg);
  RB_CHECK(cr == 0);
  if (cr != 0) return;

  check_start_geometry(spec, sig);

  {
    int p0 = sig->seq[RB_FORWARD][0];
    RB_CHECK(run_match(reg, spec, p0, 0, 0, 0, 0) ==
             sig->cost[p0].unlimited_rc);
  }

  for (int i = 0; i < sig->seq_n[RB_FORWARD]; i++) {
    int pos = sig->seq[RB_FORWARD][i];
    const Cost *c = &sig->cost[pos];
    int rc_at_limit, rc_above;
    RB_CHECK(c->cost_ok && c->retry > 0);

    rc_at_limit = run_match(reg, spec, pos, c->retry, 0, 0, 0);
    RB_CHECK(rc_at_limit == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER);
    rc_above = run_match(reg, spec, pos, c->retry + 1, 0, 0, 0);
    RB_CHECK(rc_above == c->unlimited_rc);

    /* Every observed start completes under one common match quota. */
    int rc_common = run_match(reg, spec, pos, sig->max_retry + 1, 0, 0, 0);
    RB_CHECK(rc_common == c->unlimited_rc);
  }

  RB_CHECK(sig->retry_pred.rc == ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER);
  RB_CHECK(sig->retry_pred.index == 1);
  RB_CHECK(sig->retry_pred.pos == sig->seq[RB_FORWARD][1]);
  RB_CHECK(sig->retry_actual.rc == sig->retry_pred.rc);
  RB_CHECK(sig->retry_actual.pos == sig->retry_pred.pos);
  RB_CHECK(!sig->retry_diff);
  RB_CHECK(sig->search_limit - sig->cost[sig->seq[RB_FORWARD][0]].retry <
           sig->match_limit);

  /* Both limits nonzero: local quota is reached first at the first start. */
  {
    int p0 = sig->seq[RB_FORWARD][0];
    unsigned long r0 = sig->cost[p0].retry;
    unsigned long local = r0 >= 2 ? r0 / 2 : 1;
    unsigned long huge_search = sig->max_retry * 4 + local + 100;
    SearchRun run = run_search(reg, spec, RB_FORWARD, local, huge_search);
    Pred got = actual_pred(&run);
    Pred want = predict_retry(sig->seq[RB_FORWARD], sig->seq_n[RB_FORWARD],
                              sig->cost, local, huge_search);
    RB_CHECK(want.rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER);
    RB_CHECK(want.pos == p0);
    RB_CHECK(got.rc == want.rc);
    RB_CHECK(got.pos == want.pos);
  }

  /* Equality of remaining search quota and match quota is attributed to
     the per-match limit by the library's ternary endpoint rule. */
  {
    int p0 = sig->seq[RB_FORWARD][0];
    unsigned long r0 = sig->cost[p0].retry;
    unsigned long tie = r0 >= 2 ? r0 / 2 : 1;
    SearchRun run = run_search(reg, spec, RB_FORWARD, tie, tie);
    Pred got = actual_pred(&run);
    Pred want = predict_retry(sig->seq[RB_FORWARD], sig->seq_n[RB_FORWARD],
                              sig->cost, tie, tie);
    RB_CHECK(want.rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER);
    RB_CHECK(got.rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER);
    RB_CHECK(got.pos == p0);
    RB_CHECK(got.rc == want.rc && got.pos == want.pos);
  }

  /* Reverse range uses the same costs in the opposite start order. */
  if (sig->seq_n[RB_BACKWARD] >= 2) {
    int p0 = sig->seq[RB_BACKWARD][0];
    int p1 = sig->seq[RB_BACKWARD][1];
    unsigned long r0 = sig->cost[p0].retry;
    unsigned long r1 = sig->cost[p1].retry;
    unsigned long rem = r1 >= 2 ? r1 / 2 : 1;
    unsigned long search_limit = r0 + rem;
    Pred want = predict_retry(sig->seq[RB_BACKWARD], sig->seq_n[RB_BACKWARD],
                              sig->cost, sig->max_retry + 1, search_limit);
    SearchRun run = run_search(reg, spec, RB_BACKWARD, sig->max_retry + 1,
                               search_limit);
    Pred got = actual_pred(&run);
    RB_CHECK(want.rc == ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER);
    RB_CHECK(got.rc == want.rc);
    RB_CHECK(got.pos == want.pos);
  }

  onig_free(reg);
}

static void check_call_budget_case(const rb_spec *spec,
                                  const Signature *sig) {
  OnigRegex reg;
  unsigned long old_limit = onig_get_subexp_call_limit_in_search();
  int cr = rb_compile(spec, &reg);
  RB_CHECK(cr == 0);
  if (cr != 0) return;

  check_start_geometry(spec, sig);
  RB_CHECK(onig_get_subexp_call_max_nest_level() == 20);
  RB_CHECK(spec->depth <= 5);

  {
    int p0 = sig->seq[RB_FORWARD][0];
    unsigned long half_retry = sig->cost[p0].retry >= 2
                                   ? sig->cost[p0].retry / 2
                                   : 1;
    onig_set_subexp_call_limit_in_search(0);
    RB_CHECK(run_match(reg, spec, p0, 0, 0, 0, 0) ==
             sig->cost[p0].unlimited_rc);
    RB_CHECK(sig->cost[p0].retry > 0);
    RB_CHECK(run_match(reg, spec, p0, half_retry, 0, 0, 0) ==
             ONIGERR_RETRY_LIMIT_IN_MATCH_OVER);
  }

  if (spec->kind == RB_KIND_RECURSE || spec->kind == RB_KIND_RECALL) {
    const char *ok_text = spec->kind == RB_KIND_RECALL ? "aaabbbX" : "aaabbb";
    const UChar *s = (const UChar *)ok_text;
    OnigRegion *ok_region = onig_region_new();
    int ok_rc = onig_search(reg, s, s + strlen(ok_text), s,
                            s + strlen(ok_text), ok_region, 0);
    RB_CHECK(ok_rc == 0);
    RB_CHECK(ok_region->num_regs == 2);
    RB_CHECK(ok_region->beg[0] == 0);
    RB_CHECK(ok_region->end[0] == (int)strlen(ok_text));
    RB_CHECK(ok_region->beg[1] == 0 && ok_region->end[1] == 6);
    onig_region_free(ok_region, 1);
  }

  for (int i = 0; i < sig->seq_n[RB_FORWARD]; i++) {
    int pos = sig->seq[RB_FORWARD][i];
    const Cost *c = &sig->cost[pos];
    RB_CHECK(c->cost_ok && c->calls >= 1);
    onig_set_subexp_call_limit_in_search(c->calls);
    int allowed = run_match(reg, spec, pos, 0, 0, 0, 0);
    RB_CHECK(allowed == c->unlimited_rc);
    if (c->calls >= 2) {
      onig_set_subexp_call_limit_in_search(c->calls - 1);
      int denied = run_match(reg, spec, pos, 0, 0, 0, 0);
      RB_CHECK(denied == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER);
    }
  }
  onig_set_subexp_call_limit_in_search(old_limit);

  RB_CHECK(sig->max_calls >= 2);
  RB_CHECK(sig->call_pred.rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER);
  RB_CHECK(sig->call_pred.index >= 1);
  RB_CHECK(sig->call_actual.rc == sig->call_pred.rc);
  RB_CHECK(sig->call_actual.pos == sig->call_pred.pos);
  RB_CHECK(!sig->call_diff);

  /* With G=max local calls, every start fits individually. */
  for (int i = 0; i < sig->seq_n[RB_FORWARD]; i++) {
    int pos = sig->seq[RB_FORWARD][i];
    onig_set_subexp_call_limit_in_search(sig->call_limit);
    int rc = run_match(reg, spec, pos, 0, 0, 0, 0);
    RB_CHECK(rc == sig->cost[pos].unlimited_rc);
  }
  onig_set_subexp_call_limit_in_search(old_limit);

  if (sig->seq_n[RB_BACKWARD] >= 2) {
    Pred want = predict_calls(sig->seq[RB_BACKWARD], sig->seq_n[RB_BACKWARD],
                              sig->cost, sig->max_calls);
    SearchRun run;
    Pred got;
    onig_set_subexp_call_limit_in_search(sig->max_calls);
    run = run_search(reg, spec, RB_BACKWARD, 0, 0);
    onig_set_subexp_call_limit_in_search(old_limit);
    got = actual_pred(&run);
    RB_CHECK(want.rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER ||
             want.rc == ONIG_MISMATCH);
    if (want.rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER) {
      RB_CHECK(got.rc == want.rc);
      RB_CHECK(got.pos == want.pos);
    }
  }

  if (spec->kind == RB_KIND_RECALL) {
    int p0 = sig->seq[RB_FORWARD][0];
    RB_CHECK(sig->cost[p0].calls >= 2UL * (unsigned long)(spec->depth + 1));
  }

  onig_set_subexp_call_limit_in_search(old_limit);
  onig_free(reg);
}

typedef struct {
  int rc;
  int num;
  int beg[64];
  int end[64];
} SuccessSig;

static SuccessSig region_signature(int rc, OnigRegion *r) {
  SuccessSig z;
  memset(&z, 0, sizeof(z));
  z.rc = rc;
  z.num = r ? r->num_regs : 0;
  if (r) {
    for (int i = 0; i < r->num_regs && i < 64; i++) {
      z.beg[i] = r->beg[i];
      z.end[i] = r->end[i];
    }
  }
  return z;
}

static void check_success_objects(OnigRegex reg, const rb_spec *spec,
                                  const char *text, OnigMatchParam *mp,
                                  OnigRegion *region,
                                  const SuccessSig *expected,
                                  const char *label) {
  const UChar *s = (const UChar *)text;
  int n = (int)strlen(text);
  OnigRegion *local_region = 0;
  if (!region) {
    local_region = onig_region_new();
    region = local_region;
  }
  int rc = onig_search_with_param(reg, s, s + n, s, s + n, region, 0, mp);
  SuccessSig got = region_signature(rc, region);
  RB_CHECK(rc >= 0);
  RB_CHECK(got.rc == expected->rc);
  RB_CHECK(got.num == expected->num);
  for (int i = 0; i < expected->num && i < 64; i++) {
    RB_CHECK(got.beg[i] == expected->beg[i]);
    RB_CHECK(got.end[i] == expected->end[i]);
  }
  if (local_region) onig_region_free(local_region, 1);
  if (label) (void)label;
}

static void check_reuse_case(const rb_spec *spec, const Signature *sig) {
  OnigRegex reg;
  int cr = rb_compile(spec, &reg);
  unsigned long old_m = onig_get_retry_limit_in_match();
  unsigned long old_s = onig_get_retry_limit_in_search();
  unsigned long old_c = onig_get_subexp_call_limit_in_search();
  OnigMatchParam *snapshot_mp = onig_new_match_param();
  OnigMatchParam *low_mp = onig_new_match_param();
  OnigMatchParam *fresh_mp = 0;
  OnigRegion *error_region = onig_region_new();
  const char *success_text;
  rb_spec success_model = *spec;
  rb_reference expected_ref;
  SuccessSig expected;
  int error_rc;
  SearchRun error_run;

  RB_CHECK(cr == 0 && snapshot_mp && low_mp);
  if (cr != 0) return;

  if (spec->kind == RB_KIND_RECURSE || spec->kind == RB_KIND_RECALL)
    success_text = "aaabbb";
  else
    success_text = "aaaa";

  if (spec->kind == RB_KIND_RECURSE || spec->kind == RB_KIND_RECALL) {
    success_model.depth = 3;
    success_model.suffix = 0;
    rb_refresh(&success_model);
    /* rb_refresh adds NUL suffix byte path differently; set exact text. */
    snprintf(success_model.text, sizeof(success_model.text), "aaabbb");
    success_model.text_len = 6;
  } else {
    success_model.suffix = 0;
    rb_refresh(&success_model);
    memcpy(success_model.text, "aaaa", 5);
    success_model.text_len = 4;
  }
  rb_match_reference(&success_model, 0, &expected_ref);
  RB_CHECK(expected_ref.match);
  memset(&expected, 0, sizeof(expected));
  expected.rc = expected_ref.pos;
  expected.num = expected_ref.num_regs;
  for (int i = 0; i < expected_ref.num_regs && i < 64; i++) {
    expected.beg[i] = expected_ref.cap[i].beg;
    expected.end[i] = expected_ref.cap[i].end;
  }

  /* Deliberately pollute global configuration after taking a snapshot. */
  onig_set_retry_limit_in_match(123456);
  onig_set_retry_limit_in_search(234567);
  onig_set_subexp_call_limit_in_search(7);

  if (spec->kind == RB_KIND_CALL && sig->max_calls > 0) {
    unsigned long g = sig->max_calls;
    onig_set_subexp_call_limit_in_search(g);
    onig_set_retry_limit_in_match_of_match_param(low_mp, 0);
    onig_set_retry_limit_in_search_of_match_param(low_mp, 0);
    error_run = run_search(reg, spec, RB_FORWARD, 0, 0);
    /* run_search made its own param; repeat with the shared low_mp. */
    error_rc = onig_search_with_param(
        reg, (const UChar *)spec->text,
        (const UChar *)(spec->text + spec->text_len),
        (const UChar *)spec->text,
        (const UChar *)(spec->text + spec->text_len), error_region, 0,
        low_mp);
    (void)error_run;
    RB_CHECK(error_rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER);
  } else {
    unsigned long ml = sig->max_retry ? sig->max_retry + 1 : 1000;
    unsigned long sl = sig->search_limit ? sig->search_limit : 1;
    onig_set_retry_limit_in_match_of_match_param(low_mp, ml);
    onig_set_retry_limit_in_search_of_match_param(low_mp, sl);
    error_rc = onig_search_with_param(
        reg, (const UChar *)spec->text,
        (const UChar *)(spec->text + spec->text_len),
        (const UChar *)spec->text,
        (const UChar *)(spec->text + spec->text_len), error_region, 0,
        low_mp);
    RB_CHECK(error_rc == ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER ||
             error_rc == ONIGERR_RETRY_LIMIT_IN_MATCH_OVER ||
             error_rc == ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER);
  }

  /* Restore process-global CALL configuration before proving reuse. */
  onig_set_retry_limit_in_match(old_m);
  onig_set_retry_limit_in_search(old_s);
  onig_set_subexp_call_limit_in_search(old_c);

  /* Same regex, same param object and the region that received the error. */
  check_success_objects(reg, spec, success_text, low_mp, error_region,
                        &expected, "same-low");

  fresh_mp = onig_new_match_param();
  check_success_objects(reg, spec, success_text, fresh_mp,
                        NULL, &expected, "fresh");
  check_success_objects(reg, spec, success_text, snapshot_mp,
                        NULL, &expected, "snapshot");

  /* Alternating parameter objects must not carry request counters. */
  check_success_objects(reg, spec, success_text, low_mp, error_region,
                        &expected, "low-again");
  check_success_objects(reg, spec, success_text, fresh_mp,
                        NULL, &expected, "fresh-again");
  check_success_objects(reg, spec, success_text, snapshot_mp,
                        NULL, &expected, "snapshot-again");

  onig_set_retry_limit_in_match(old_m);
  onig_set_retry_limit_in_search(old_s);
  onig_set_subexp_call_limit_in_search(old_c);
  RB_CHECK(onig_get_retry_limit_in_match() == old_m);
  RB_CHECK(onig_get_retry_limit_in_search() == old_s);
  RB_CHECK(onig_get_subexp_call_limit_in_search() == old_c);

  /* Order exchange: fresh/default/low gives the same answer. */
  check_success_objects(reg, spec, success_text, fresh_mp,
                        NULL, &expected, "order-fresh");
  check_success_objects(reg, spec, success_text, snapshot_mp,
                        NULL, &expected, "order-snapshot");
  check_success_objects(reg, spec, success_text, low_mp, error_region,
                        &expected, "order-low");

  onig_free_match_param(snapshot_mp);
  onig_free_match_param(low_mp);
  onig_free_match_param(fresh_mp);
  onig_region_free(error_region, 1);
  onig_free(reg);
}

int redos_budget_case(const char *group, unsigned seed, int case_id) {
  const char *replay_spec = getenv("REDOS_REPLAY_SPEC");
  rb_onig_init();
  memset(&g_hub, 0, sizeof(g_hub));
  g_hub.replay = replay_spec && *replay_spec;

  if (g_hub.replay) {
    rb_spec replay;
    Signature sig;
    int rr = rb_read_spec(&replay, replay_spec);
    RB_CHECK(rr == 0);
    if (rr == 0) {
      g_hub.spec = replay;
      snprintf(g_hub.dir, sizeof(g_hub.dir), "%s", replay_spec);
      sig = evaluate_signature(&replay);
      g_hub.sig = sig;
      g_hub.has_sig = 1;
      if (replay.plan == RB_PLAN_CALL_TOTAL) {
        RB_CHECK(sig.call_pred.rc ==
                 ONIGERR_SUBEXP_CALL_LIMIT_IN_SEARCH_OVER);
        RB_CHECK(sig.call_actual.rc == sig.call_pred.rc);
        RB_CHECK(sig.call_actual.pos == sig.call_pred.pos);
      } else {
        RB_CHECK(sig.retry_pred.rc ==
                 ONIGERR_RETRY_LIMIT_IN_SEARCH_OVER);
        RB_CHECK(sig.retry_actual.rc == sig.retry_pred.rc);
        RB_CHECK(sig.retry_actual.pos == sig.retry_pred.pos);
      }
    }
    return 0;
  }

  rb_generate(&g_hub.spec, group, seed, case_id);
  RB_CHECK(g_hub.spec.allowed);
  if (!g_hub.spec.allowed) return 0;

  if (strcmp(group, "corpus") == 0) {
    check_corpus_case(&g_hub.spec);
  } else if (strcmp(group, "search") == 0) {
    Signature sig = evaluate_signature(&g_hub.spec);
    g_hub.sig = sig;
    g_hub.has_sig = 1;
    check_retry_budget_case(&g_hub.spec, &sig);
  } else if (strcmp(group, "calls") == 0) {
    Signature sig = evaluate_signature(&g_hub.spec);
    g_hub.sig = sig;
    g_hub.has_sig = 1;
    check_call_budget_case(&g_hub.spec, &sig);
  } else if (strcmp(group, "reuse") == 0) {
    Signature sig = evaluate_signature(&g_hub.spec);
    g_hub.sig = sig;
    g_hub.has_sig = 1;
    check_reuse_case(&g_hub.spec, &sig);
  } else {
    RB_CHECK(0 && "unknown budget group");
  }
  return 0;
}
