#include "redos_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t rb_splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

unsigned rb_rand_range(uint64_t *state, unsigned limit) {
  if (limit == 0) return 0;
  return (unsigned)(rb_splitmix64(state) % limit);
}

unsigned rb_seed_for(const char *group, unsigned seed, int case_id) {
  uint64_t h = 1469598103934665603ULL;
  const unsigned char *p = (const unsigned char *)(group ? group : "");
  while (*p) {
    h ^= *p++;
    h *= 1099511628211ULL;
  }
  h ^= (uint64_t)seed + 0x9e3779b97f4a7c15ULL;
  h ^= (uint64_t)(case_id + 1) * 0xd1b54a32d192ed03ULL;
  return (unsigned)(h ^ (h >> 32));
}

const char *rb_kind_name(int kind) {
  switch (kind) {
    case RB_KIND_BRANCH: return "branch-repeat";
    case RB_KIND_CALL: return "called-branch-body";
    case RB_KIND_RECURSE: return "bounded-recursion";
    case RB_KIND_RECALL: return "recall-after-backtrack";
    default: return "unknown";
  }
}

const char *rb_direction_name(int direction) {
  return direction == RB_BACKWARD ? "backward" : "forward";
}

static void copy_frag(rb_spec *s, int index, const char *value) {
  size_t n = strlen(value);
  if (n > RB_MAX_FRAG) n = RB_MAX_FRAG;
  memcpy(s->frag[index], value, n);
  s->frag[index][n] = 0;
  s->frag_len[index] = (int)n;
}

void rb_generate(rb_spec *s, const char *group, unsigned seed, int case_id) {
  static const char *sets[][2] = {
      {"a", "aa"}, {"aa", "aaa"}, {"a", "aaa"}, {"aa", "aaaa"}};
  static const char suffixes[] = "!#?";
  uint64_t state = rb_seed_for(group, seed, case_id);
  unsigned pick;

  memset(s, 0, sizeof(*s));
  snprintf(s->group, sizeof(s->group), "%s", group ? group : "corpus");
  s->seed = seed;
  s->case_id = case_id;
  s->observe = 1;
  s->nf = 2;
  s->suffix = '!';
  s->direction = RB_FORWARD;

  if (strcmp(group, "calls") == 0) {
    if (case_id < 16) s->kind = RB_KIND_CALL;
    else if (case_id < 20) s->kind = RB_KIND_RECURSE;
    else s->kind = RB_KIND_RECALL;
    s->plan = RB_PLAN_CALL_TOTAL;
  } else if (strcmp(group, "search") == 0) {
    s->kind = RB_KIND_BRANCH;
    s->plan = RB_PLAN_RETRY_TOTAL;
  } else if (strcmp(group, "reuse") == 0) {
    static const int kinds[] = {RB_KIND_BRANCH, RB_KIND_CALL, RB_KIND_RECURSE};
    s->kind = kinds[case_id % 3];
    s->plan = (s->kind == RB_KIND_CALL) ? RB_PLAN_CALL_TOTAL
                                        : RB_PLAN_RETRY_TOTAL;
  } else {
    static const int kinds[] = {RB_KIND_BRANCH, RB_KIND_CALL,
                                RB_KIND_RECURSE, RB_KIND_RECALL};
    s->kind = kinds[case_id % 4];
    s->plan = RB_PLAN_NONE;
  }

  if (case_id == 0 && (strcmp(group, "search") == 0 ||
                       strcmp(group, "calls") == 0 ||
                       strcmp(group, "reuse") == 0)) {
    if (strcmp(group, "calls") == 0) s->kind = RB_KIND_CALL;
    if (strcmp(group, "reuse") == 0) s->kind = RB_KIND_BRANCH;
    s->lo = 1;
    s->hi = 8;
    s->na = 6;
    s->depth = 3;
    s->suffix = '!';
    s->order_swapped = 0;
    copy_frag(s, 0, "a");
    copy_frag(s, 1, "aa");
    rb_refresh(s);
    return;
  }

  pick = rb_rand_range(&state, sizeof(sets) / sizeof(sets[0]));
  copy_frag(s, 0, sets[pick][0]);
  copy_frag(s, 1, sets[pick][1]);
  s->order_swapped = rb_rand_range(&state, 2);
  if (s->order_swapped) {
    char tmp[RB_MAX_FRAG + 1];
    int tn = s->frag_len[0];
    memcpy(tmp, s->frag[0], sizeof(tmp));
    copy_frag(s, 0, s->frag[1]);
    copy_frag(s, 1, tmp);
    (void)tn;
  }

  s->lo = 1 + rb_rand_range(&state, 2);
  s->hi = s->lo + 3 + rb_rand_range(&state, 5);
  s->na = 5 + rb_rand_range(&state, 12);
  s->depth = 2 + rb_rand_range(&state, 4);
  s->suffix = suffixes[rb_rand_range(&state, sizeof(suffixes) - 1)];
  s->direction = rb_rand_range(&state, 2);

  if (s->kind == RB_KIND_RECURSE || s->kind == RB_KIND_RECALL) {
    s->na = s->depth;
    s->hi = s->depth;
    s->lo = 1;
  }
  rb_refresh(s);
  while (!s->allowed && s->hi > s->lo) {
    s->hi--;
    rb_refresh(s);
  }
  if (!s->allowed && s->na > 8) {
    s->na = 8;
    rb_refresh(s);
  }
}

int rb_build_pattern(const rb_spec *s, int with_callout, char *out, size_t size) {
  int n = 0;
  const char *co = with_callout ? "(?{rb})" : "";
  if (s->kind == RB_KIND_BRANCH) {
    if (s->nf == 1)
      n = snprintf(out, size, "%s((%s){%d,%d})\\z", co, s->frag[0], s->lo, s->hi);
    else
      n = snprintf(out, size, "%s((%s|%s){%d,%d})\\z", co, s->frag[0],
                   s->frag[1], s->lo, s->hi);
  } else if (s->kind == RB_KIND_CALL) {
    if (s->nf == 1)
      n = snprintf(out, size,
                   "%s(?<item>%s){0}(?:\\g<item>){%d,%d}\\z", co,
                   s->frag[0], s->lo, s->hi);
    else
      n = snprintf(out, size,
                   "%s(?<item>%s|%s){0}(?:\\g<item>){%d,%d}\\z", co,
                   s->frag[0], s->frag[1], s->lo, s->hi);
  } else if (s->kind == RB_KIND_RECURSE) {
    n = snprintf(out, size, "%s(?<r>a\\g<r>?b){0}\\g<r>\\z", co);
  } else {
    n = snprintf(out, size,
                 "%s(?<r>a\\g<r>?b){0}(?:\\g<r>X|\\g<r>Y)\\z", co);
  }
  if (n < 0 || (size_t)n >= size) return -1;
  return n;
}

unsigned long long rb_expansion_bound(const rb_spec *s) {
  unsigned long long ways = 1, total = 1;
  int k;
  if (s->kind == RB_KIND_RECURSE || s->kind == RB_KIND_RECALL)
    return 64ULL * (unsigned long long)(s->depth + 1);
  for (k = 1; k <= s->hi && k <= 20; k++) {
    ways *= (unsigned long long)(s->nf ? s->nf : 1);
    total += ways;
    if (total > (1ULL << 48)) return (1ULL << 48);
  }
  return total * 32ULL + (unsigned long long)s->text_len;
}

int rb_spec_within_limits(const rb_spec *s) {
  return s->text_len <= RB_MAX_TEXT && s->depth <= 5 &&
         s->expansion <= RB_BRANCH_ATTEMPT_LIMIT &&
         s->frag_len[0] <= RB_MAX_FRAG && s->frag_len[1] <= RB_MAX_FRAG;
}

void rb_refresh(rb_spec *s) {
  int i;
  if (s->kind == RB_KIND_RECURSE || s->kind == RB_KIND_RECALL) {
    int d = s->depth < 1 ? 1 : (s->depth > 5 ? 5 : s->depth);
    s->depth = d;
    s->text_len = 2 * d + 1;
    for (i = 0; i < d; i++) s->text[i] = 'a';
    for (; i < 2 * d; i++) s->text[i] = 'b';
    s->text[2 * d] = s->suffix ? s->suffix : '!';
    s->text[s->text_len] = 0;
  } else {
    if (s->na < 0) s->na = 0;
    if (s->na > RB_MAX_TEXT - 1) s->na = RB_MAX_TEXT - 1;
    s->text_len = s->na + 1;
    for (i = 0; i < s->na; i++) s->text[i] = 'a';
    s->text[s->na] = s->suffix ? s->suffix : '!';
    s->text[s->text_len] = 0;
  }
  for (i = 0; i < 2; i++) s->frag_len[i] = (int)strlen(s->frag[i]);
  s->pat_len = rb_build_pattern(s, 0, s->pat, sizeof(s->pat));
  s->expansion = rb_expansion_bound(s);
  s->allowed = rb_spec_within_limits(s);
}

static int lit_at(const rb_spec *s, int pos, int fi) {
  int len = s->frag_len[fi];
  if (pos < 0 || pos + len > s->text_len) return 0;
  return memcmp(s->text + pos, s->frag[fi], len) == 0;
}

static void set_ok(rb_reference *r, int pos, int end, int num) {
  r->match = 1;
  r->pos = pos;
  r->end = end;
  r->num_regs = num;
  r->cap[0].beg = pos;
  r->cap[0].end = end;
}

struct branch_ctx {
  const rb_spec *s;
  int at;
  rb_reference *out;
};

static int branch_repeat(struct branch_ctx *cx, int pos, int count,
                         int last_beg, int last_end) {
  const rb_spec *s = cx->s;
  int fi;
  if (count < s->hi) {
    for (fi = 0; fi < s->nf; fi++) {
      int len = s->frag_len[fi];
      if (!lit_at(s, pos, fi)) continue;
      if (branch_repeat(cx, pos + len, count + 1, pos, pos + len))
        return 1;
    }
  }
  if (count >= s->lo && pos == s->text_len) {
    set_ok(cx->out, cx->at, pos, s->kind == RB_KIND_BRANCH ? 3 : 2);
    if (s->kind == RB_KIND_BRANCH) {
      cx->out->cap[1].beg = cx->at;
      cx->out->cap[1].end = pos;
      cx->out->cap[2].beg = last_beg;
      cx->out->cap[2].end = last_end;
    } else {
      cx->out->cap[1].beg = last_beg;
      cx->out->cap[1].end = last_end;
    }
    return 1;
  }
  return 0;
}

static int rec_dfs(const rb_spec *s, int pos, int nest, int *end_out) {
  int after_a, nested_end;
  if (nest > 5 || pos >= s->text_len || s->text[pos] != 'a') return 0;
  after_a = pos + 1;

  /* Greedy optional call: try R first, then its required close byte. */
  if (rec_dfs(s, after_a, nest + 1, &nested_end)) {
    if (nested_end < s->text_len && s->text[nested_end] == 'b') {
      *end_out = nested_end + 1;
      return 1;
    }
  }
  /* Backtracked path: omit the optional call and close this frame. */
  if (after_a < s->text_len && s->text[after_a] == 'b') {
    *end_out = after_a + 1;
    return 1;
  }
  return 0;
}

void rb_match_reference(const rb_spec *s, int pos, rb_reference *result) {
  int end = -1;
  memset(result, 0, sizeof(*result));
  result->pos = -1;
  result->end = -1;
  for (int i = 0; i < RB_MAX_CAPTURES; i++)
    result->cap[i].beg = result->cap[i].end = -1;

  if (pos < 0 || pos > s->text_len) return;

  if (s->kind == RB_KIND_BRANCH || s->kind == RB_KIND_CALL) {
    struct branch_ctx cx;
    cx.s = s;
    cx.at = pos;
    cx.out = result;
    branch_repeat(&cx, pos, 0, -1, -1);
  } else if (s->kind == RB_KIND_RECURSE) {
    if (rec_dfs(s, pos, 1, &end) && end == s->text_len) {
      set_ok(result, pos, end, 2);
      result->cap[1].beg = pos;
      result->cap[1].end = end;
    }
  } else {
    static const char tails[2] = {'X', 'Y'};
    int alt;
    for (alt = 0; alt < 2; alt++) {
      int rend;
      if (rec_dfs(s, pos, 1, &rend) && rend < s->text_len &&
          s->text[rend] == tails[alt] && rend + 1 == s->text_len) {
        set_ok(result, pos, rend + 1, 2);
        result->cap[1].beg = pos;
        result->cap[1].end = rend;
        break;
      }
    }
  }
}

rb_reference rb_reference_search(const rb_spec *s) {
  rb_reference r;
  int pos;
  for (pos = 0; pos <= s->text_len; pos++) {
    rb_match_reference(s, pos, &r);
    if (r.match) return r;
  }
  memset(&r, 0, sizeof(r));
  r.pos = -1;
  r.end = -1;
  return r;
}

int rb_hex_encode(const unsigned char *data, int len, char *out, size_t out_size) {
  static const char h[] = "0123456789abcdef";
  int i;
  if (out_size < (size_t)len * 2 + 1) return -1;
  for (i = 0; i < len; i++) {
    out[i * 2] = h[(data[i] >> 4) & 15];
    out[i * 2 + 1] = h[data[i] & 15];
  }
  out[len * 2] = 0;
  return len * 2;
}

int rb_hex_decode(const char *hex, unsigned char *out, size_t out_size) {
  int n = 0;
  while (hex[0] && hex[1]) {
    unsigned v;
    char pair[3] = {hex[0], hex[1], 0};
    if (sscanf(pair, "%2x", &v) != 1) return -1;
    if ((size_t)n >= out_size) return -1;
    out[n++] = (unsigned char)v;
    hex += 2;
  }
  return n;
}

static void write_hex_key(FILE *f, const char *key, const char *s) {
  char hex[RB_MAX_PAT * 2 + 4];
  int n = (int)strlen(s);
  rb_hex_encode((const unsigned char *)s, n, hex, sizeof(hex));
  fprintf(f, "%s=%s\n", key, hex);
}

int rb_write_spec(const rb_spec *s, const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) return -1;
  fprintf(f, "RB_BUDGET_SPEC=1\n");
  fprintf(f, "group=%s\n", s->group);
  fprintf(f, "seed=%u\n", s->seed);
  fprintf(f, "case_id=%d\n", s->case_id);
  fprintf(f, "kind=%d\n", s->kind);
  fprintf(f, "f0=%s\nf1=%s\nnf=%d\norder=%d\n", s->frag[0], s->frag[1],
          s->nf, s->order_swapped);
  fprintf(f, "lo=%d\nhi=%d\nna=%d\nsuffix=%d\ndepth=%d\n", s->lo, s->hi,
          s->na, (unsigned char)s->suffix, s->depth);
  fprintf(f, "direction=%d\nplan=%d\nobserve=%d\n", s->direction, s->plan,
          s->observe);
  fprintf(f, "expansion=%llu\nallowed=%d\n", s->expansion, s->allowed);
  write_hex_key(f, "pat_hex", s->pat);
  write_hex_key(f, "text_hex", s->text);
  fclose(f);
  return 0;
}

static void get_key(char *line, const char *key, char *value, size_t size) {
  size_t n = strlen(key);
  value[0] = 0;
  if (strncmp(line, key, n) != 0 || line[n] != '=') return;
  snprintf(value, size, "%s", line + n + 1);
  value[strcspn(value, "\r\n")] = 0;
}

int rb_read_spec(rb_spec *s, const char *path) {
  FILE *f = fopen(path, "r");
  char line[RB_MAX_PAT * 2 + 16];
  char value[RB_MAX_PAT * 2 + 16];
  int got_pat = 0, got_text = 0;
  if (!f) return -1;
  memset(s, 0, sizeof(*s));
  s->nf = 2;
  while (fgets(line, sizeof(line), f)) {
    get_key(line, "group", value, sizeof(value));
    if (value[0]) snprintf(s->group, sizeof(s->group), "%s", value);
    get_key(line, "seed", value, sizeof(value));
    if (value[0]) s->seed = (unsigned)strtoul(value, 0, 10);
    get_key(line, "case_id", value, sizeof(value));
    if (value[0]) s->case_id = atoi(value);
    get_key(line, "kind", value, sizeof(value));
    if (value[0]) s->kind = atoi(value);
    get_key(line, "f0", value, sizeof(value));
    if (value[0]) copy_frag(s, 0, value);
    get_key(line, "f1", value, sizeof(value));
    if (value[0]) copy_frag(s, 1, value);
    get_key(line, "nf", value, sizeof(value));
    if (value[0]) s->nf = atoi(value);
    get_key(line, "order", value, sizeof(value));
    if (value[0]) s->order_swapped = atoi(value);
    get_key(line, "lo", value, sizeof(value));
    if (value[0]) s->lo = atoi(value);
    get_key(line, "hi", value, sizeof(value));
    if (value[0]) s->hi = atoi(value);
    get_key(line, "na", value, sizeof(value));
    if (value[0]) s->na = atoi(value);
    get_key(line, "suffix", value, sizeof(value));
    if (value[0]) s->suffix = (char)atoi(value);
    get_key(line, "depth", value, sizeof(value));
    if (value[0]) s->depth = atoi(value);
    get_key(line, "direction", value, sizeof(value));
    if (value[0]) s->direction = atoi(value);
    get_key(line, "plan", value, sizeof(value));
    if (value[0]) s->plan = atoi(value);
    get_key(line, "observe", value, sizeof(value));
    if (value[0]) s->observe = atoi(value);
    get_key(line, "pat_hex", value, sizeof(value));
    if (value[0]) {
      int n = rb_hex_decode(value, (unsigned char *)s->pat, sizeof(s->pat) - 1);
      if (n >= 0) { s->pat[n] = 0; s->pat_len = n; got_pat = 1; }
    }
    get_key(line, "text_hex", value, sizeof(value));
    if (value[0]) {
      int n = rb_hex_decode(value, (unsigned char *)s->text, sizeof(s->text) - 1);
      if (n >= 0) { s->text[n] = 0; s->text_len = n; got_text = 1; }
    }
    value[0] = 0;
  }
  fclose(f);
  if (!got_pat || !got_text) {
    rb_refresh(s);
    return -1;
  }
  s->expansion = rb_expansion_bound(s);
  s->allowed = rb_spec_within_limits(s);
  return 0;
}
