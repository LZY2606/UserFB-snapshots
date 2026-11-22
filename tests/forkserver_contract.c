/*
  Copyright 2017 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   Contract tests for the afl-tmin / afl-analyze -F forkserver backend.

   Every case drives the real tool binaries against the deterministic local
   target in tests/forkserver_target.c and asserts observable process and
   protocol behavior - nothing here mocks the pipe protocol. The five entry
   points match the declarations in support/contract-main.c; zero means the
   case passed. All temporary files and children are cleaned up.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* One parsed event-log line emitted by forkserver_target.c:
   KIND pid=PID iter=IT len=LEN bytes=HEX */

struct log_entry {

  char kind[24];
  long pid;
  unsigned iter;
  size_t len;
  unsigned char bytes[4096];

};

static char g_tmpl[] = "/tmp/claude/fst-contract.XXXXXX";
/* g_tmpl path is rewritten at runtime into $TMPDIR when available. */

static char* make_work(void) {

  const char *base = getenv("TMPDIR");
  char *dir;

  if (base && *base) {

    size_t n = strlen(base) + 32;
    dir = malloc(n);
    snprintf(dir, n, "%s/fst-contract.XXXXXX", base);

  } else {

    dir = strdup(g_tmpl);

  }

  if (!mkdtemp(dir)) {

    free(dir);
    return NULL;

  }

  return dir;

}


static void remove_work(char *dir) {

  if (!dir) return;

  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  if (system(cmd)) { /* best effort cleanup */ }
  free(dir);

}


static char* path_join(char *buf, size_t n, const char *dir, const char *name) {

  snprintf(buf, n, "%s/%s", dir, name);
  return buf;

}


static void write_file(const char *path, const void *data, size_t len) {

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

  if (fd < 0) { perror("open"); _exit(90); }

  size_t off = 0;
  const unsigned char *p = data;

  while (off < len) {

    ssize_t n = write(fd, p + off, len - off);
    if (n <= 0) _exit(91);
    off += (size_t)n;

  }

  close(fd);

}


static int hex_val(int c) {

  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;

}


/* Run a command, capturing combined output. Returns the raw exit status
   interpretation: the child exit code, or -1 on failure to launch. */

static int run_capture(const char *const argv[], const char *out_path,
                       const char *log_path) {

  pid_t pid = fork();

  if (pid < 0) return -1;

  if (!pid) {

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) _exit(92);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);

    if (log_path) setenv("FST_LOG", log_path, 1);

    execv(argv[0], (char *const *)argv);
    _exit(127);

  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;

}


static int file_contains(const char *path, const char *needle) {

  FILE *f = fopen(path, "r");
  if (!f) return 0;

  char line[2048];

  while (fgets(line, sizeof(line), f))
    if (strstr(line, needle)) { fclose(f); return 1; }

  fclose(f);
  return 0;

}


static int file_equals(const char *a, const char *b) {

  FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
  if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }

  int eq = 1;
  int ca, cb;

  do {

    ca = fgetc(fa);
    cb = fgetc(fb);
    if (ca != cb) { eq = 0; break; }

  } while (ca != EOF);

  fclose(fa);
  fclose(fb);
  return eq;

}


/* Parse the target event log. Returns the number of entries, or -1 on error.
   Entries beyond cap are counted but not stored. */

static int parse_log(const char *path, struct log_entry *out, int cap) {

  FILE *f = fopen(path, "r");
  if (!f) return -1;

  char line[8200];
  int total = 0;

  while (fgets(line, sizeof(line), f)) {

    struct log_entry e;
    char hex[8192];
    char kind[24];
    long pid;
    unsigned iter;
    size_t len;
    int fields;

    memset(&e, 0, sizeof(e));
    hex[0] = 0;

    fields = sscanf(line, "%23s pid=%ld iter=%u len=%zu bytes=%8191s",
                    kind, &pid, &iter, &len, hex);

    if (fields < 4) continue;

    strncpy(e.kind, kind, sizeof(e.kind) - 1);
    e.pid = pid;
    e.iter = iter;
    e.len = len;

    size_t hlen = strlen(hex);
    size_t decoded = hlen / 2;

    for (size_t i = 0; i + 1 < hlen && i / 2 < sizeof(e.bytes); i += 2) {

      int hi = hex_val((unsigned char)hex[i]);
      int lo = hex_val((unsigned char)hex[i + 1]);
      if (hi < 0 || lo < 0) { decoded = (size_t)-1; break; }
      e.bytes[i / 2] = (unsigned char)((hi << 4) | lo);

    }

    if (decoded != len) { fclose(f); return -1; }

    if (total < cap) out[total] = e;
    total++;

  }

  fclose(f);
  return total;

}


static int count_kind(const struct log_entry *e, int n, const char *kind) {

  int c = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(e[i].kind, kind)) c++;
  return c;

}


/* Case 1: stdin replay through a persistent worker. Proves one init, process
   reuse, exact per-run bytes with EOF (including shorter and empty
   candidates), and no stale bytes from previous runs. */

int test_stdin_replay(const char *tmin, const char *analyze,
                      const char *target) {

  (void)analyze;

  char *w = make_work();
  if (!w) return 10;

  char in[256], out[256], logp[256], cap[256];
  path_join(in, sizeof(in), w, "in");
  path_join(out, sizeof(out), w, "out");
  path_join(logp, sizeof(logp), w, "log");
  path_join(cap, sizeof(cap), w, "cap");

  /* Repeated bytes exercise the identical-block fast path too. The prefix
     target's coverage depends only on the first byte, so this shrinks to a
     single-byte case and deterministically visits several shorter lengths. */
  write_file(in, "ABABABAB", 8);

  const char *argv[] = {
    tmin, "-F", "-i", in, "-o", out, "-m", "none", "-t", "1000",
    "--", target, "pref", NULL
  };

  int rc = run_capture(argv, cap, logp);
  if (rc) { remove_work(w); return 11; }

  struct log_entry e[256];
  int n = parse_log(logp, e, 256);
  if (n < 2) { remove_work(w); return 12; }

  if (count_kind(e, n, "INIT") != 1) { remove_work(w); return 13; }

  /* Persistent mode: every executed candidate shares one process. */
  long run_pid = -1;
  int runs = 0, min_len = 1 << 30, max_len = 0;

  for (int i = 0; i < n; i++) {

    if (strcmp(e[i].kind, "RUN")) continue;
    runs++;

    if (run_pid < 0) run_pid = e[i].pid;
    if (e[i].pid != run_pid) { remove_work(w); return 14; }

    if ((size_t)e[i].len < (size_t)min_len) min_len = (int)e[i].len;
    if (e[i].len > (size_t)max_len) max_len = (int)e[i].len;

  }

  if (!runs) { remove_work(w); return 15; }
  if (max_len != 8) { remove_work(w); return 16; }

  /* Observing a run whose read length is strictly below a preceding longer
     run proves truncation: without it, that read would have returned the
     previous, longer contents and never hit EOF. parse_log() already
     verified each run's decoded byte count equals its logged length. */
  if (min_len >= max_len) { remove_work(w); return 17; }

  /* And the minimized output must be the single prefix byte the coverage
     depends on, read back exactly. */
  struct stat st1;
  if (stat(out, &st1) || st1.st_size != 1) { remove_work(w); return 18; }

  unsigned char got = 0;
  int ofd = open(out, O_RDONLY);
  if (ofd < 0 || read(ofd, &got, 1) != 1 || got != 'A') {
    if (ofd >= 0) close(ofd);
    remove_work(w); return 19;
  }
  close(ofd);

  /* Empty candidate: a target that crashes on every input lets tmin delete
     all the way down to zero bytes. The empty RUN must still reach EOF. */
  char in2[256], out2[256], log2[256], cap2[256];
  path_join(in2, sizeof(in2), w, "in2");
  path_join(out2, sizeof(out2), w, "out2");
  path_join(log2, sizeof(log2), w, "log2");
  path_join(cap2, sizeof(cap2), w, "cap2");

  write_file(in2, "QQ", 2);

  const char *argv2[] = {
    tmin, "-F", "-i", in2, "-o", out2, "-m", "none", "-t", "1000",
    "--", target, "crashall", NULL
  };

  if (run_capture(argv2, cap2, log2)) { remove_work(w); return 19; }

  struct log_entry e2[256];
  int n2 = parse_log(log2, e2, 256);
  if (n2 < 2) { remove_work(w); return 20; }

  int saw_empty = 0;
  for (int i = 0; i < n2; i++)
    if (!strcmp(e2[i].kind, "RUN") && e2[i].len == 0) saw_empty = 1;

  if (!saw_empty) { remove_work(w); return 21; }

  /* Output of the all-crashing minimization is genuinely empty. */
  struct stat st;
  if (stat(out2, &st) || st.st_size != 0) { remove_work(w); return 22; }

  remove_work(w);
  return 0;

}


/* Case 2: file-based replay via @@ and -f. The target opens the candidate
   path itself; truncation between runs must still give exact bytes. */

int test_file_replay(const char *tmin, const char *analyze,
                     const char *target) {

  (void)analyze;
  char *w = make_work();
  if (!w) return 30;

  char in[256], out[256], logp[256], cap[256], ff[256];
  path_join(in, sizeof(in), w, "in");
  path_join(out, sizeof(out), w, "out");
  path_join(logp, sizeof(logp), w, "log");
  path_join(cap, sizeof(cap), w, "cap");
  path_join(ff, sizeof(ff), w, "feed");

  write_file(in, "ABCDABCD", 8);

  const char *argv[] = {
    tmin, "-F", "-i", in, "-o", out, "-m", "none", "-t", "1000",
    "--", target, "pref", "@@", NULL
  };

  if (run_capture(argv, cap, logp)) { remove_work(w); return 31; }

  struct log_entry e[256];
  int n = parse_log(logp, e, 256);
  if (n < 2) { remove_work(w); return 32; }
  if (count_kind(e, n, "INIT") != 1) { remove_work(w); return 33; }

  long pid = -1;
  int runs = 0, min_len = 1 << 30;

  for (int i = 0; i < n; i++) {

    if (strcmp(e[i].kind, "RUN")) continue;
    runs++;
    if (pid < 0) pid = e[i].pid;
    if (e[i].pid != pid) { remove_work(w); return 34; }
    if ((int)e[i].len < min_len) min_len = (int)e[i].len;

  }

  if (!runs || min_len >= 8) { remove_work(w); return 35; }

  /* -f with an explicit feed file: result must match the direct (non -F)
     execution path byte for byte. */
  char out_d[256], out_f[256], cap_d[256], cap_f[256], log_d[256];
  path_join(out_d, sizeof(out_d), w, "od");
  path_join(out_f, sizeof(out_f), w, "of");
  path_join(cap_d, sizeof(cap_d), w, "cd");
  path_join(cap_f, sizeof(cap_f), w, "cf");
  path_join(log_d, sizeof(log_d), w, "ld");

  const char *direct[] = {
    tmin, "-f", ff, "-i", in, "-o", out_d, "-m", "none", "-t", "1000",
    "--", target, "pref", "@@", NULL
  };
  const char *forked[] = {
    tmin, "-F", "-f", ff, "-i", in, "-o", out_f, "-m", "none", "-t", "1000",
    "--", target, "pref", "@@", NULL
  };

  if (run_capture(direct, cap_d, log_d)) { remove_work(w); return 36; }
  if (run_capture(forked, cap_f, NULL)) { remove_work(w); return 37; }

  if (!file_equals(out_d, out_f)) { remove_work(w); return 38; }

  remove_work(w);
  return 0;

}


/* Case 3: a persistent worker that hangs must be killed, reported as a
   timeout, and replaced by a fresh worker for the next candidates. A clean
   SIGSTOP loop boundary must never look like a crash. */

int test_persistent_recovery(const char *tmin, const char *analyze,
                             const char *target) {

  (void)analyze;
  char *w = make_work();
  if (!w) return 40;

  char in[256], out[256], logp[256], cap[256];
  path_join(in, sizeof(in), w, "in");
  path_join(out, sizeof(out), w, "out");
  path_join(logp, sizeof(logp), w, "log");
  path_join(cap, sizeof(cap), w, "cap");

  /* Block normalization first rewrites the leading block to '0', producing
     "0ANGx" (no hang); a later deletion step yields "HANGx" exactly and
     triggers one deterministic timeout, after which minimization continues. */
  write_file(in, "HANGx", 5);

  const char *argv[] = {
    tmin, "-F", "-i", in, "-o", out, "-m", "none", "-t", "200",
    "--", target, "persist", NULL
  };

  int rc = run_capture(argv, cap, logp);
  if (rc) { remove_work(w); return 41; }

  if (!file_contains(cap, "hang=") ||
      file_contains(cap, "hang=0")) { remove_work(w); return 42; }

  struct log_entry e[256];
  int n = parse_log(logp, e, 256);
  if (n < 2) { remove_work(w); return 43; }

  /* At least two distinct worker PIDs: the hung one was replaced. */
  long first = -1;
  int distinct = 0;
  int saw_run_after = 0;

  for (int i = 0; i < n; i++) {

    if (strcmp(e[i].kind, "RUN")) continue;

    if (first < 0) first = e[i].pid;
    int known = 0;
    for (int j = 0; j < i; j++)
      if (!strcmp(e[j].kind, "RUN") && e[j].pid == e[i].pid) known = 1;
    if (!known) distinct++;
    if (e[i].pid != first) saw_run_after = 1;

  }

  if (distinct < 2 || !saw_run_after) { remove_work(w); return 44; }

  /* A normal persistent run that stops at the loop boundary is not a crash:
     tmin stays in instrumented mode on an ordinary input. */
  char in2[256], out2[256], log2[256], cap2[256];
  path_join(in2, sizeof(in2), w, "in2");
  path_join(out2, sizeof(out2), w, "out2");
  path_join(log2, sizeof(log2), w, "log2");
  path_join(cap2, sizeof(cap2), w, "cap2");

  write_file(in2, "ABCDEFGH", 8);

  const char *argv2[] = {
    tmin, "-F", "-i", in2, "-o", out2, "-m", "none", "-t", "1000",
    "--", target, "persist", NULL
  };

  if (run_capture(argv2, cap2, log2)) { remove_work(w); return 45; }

  if (!file_contains(cap2, "instrumented")) { remove_work(w); return 46; }
  /* "crash mode" is only printed when the dry run is classified as a crash;
     the ordinary "path=N crash=0 hang=0" summary must not trip this. */
  if (file_contains(cap2, "crash mode")) { remove_work(w); return 47; }

  remove_work(w);
  return 0;

}


/* Normalize tool output for comparison: drop ANSI escapes and the -F-only
   status lines. */

static void normalize_copy(const char *src, const char *dst) {

  FILE *in = fopen(src, "r"), *out = fopen(dst, "w");
  if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return; }

  int c;
  while ((c = fgetc(in)) != EOF) {

    if (c == 0x1b) {

      /* Consume an ANSI escape sequence. */
      int d = fgetc(in);
      if (d != '[') { if (d != EOF) { /* skip */ } continue; }
      while ((d = fgetc(in)) != EOF && !(d >= '@' && d <= '~')) { }
      continue;

    }

    fputc(c, out);

  }

  fclose(in);
  fclose(out);

  /* Filter the -F-only lines in a second pass. */
  FILE *a = fopen(dst, "r");
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s.n", dst);
  FILE *b = fopen(tmp, "w");
  if (!a || !b) { if (a) fclose(a); if (b) fclose(b); return; }

  char line[2048];
  while (fgets(line, sizeof(line), a)) {

    if (strstr(line, "Persistent mode binary detected") ||
        strstr(line, "Deferred forkserver binary detected") ||
        strstr(line, "fork server is up")) continue;
    fputs(line, b);

  }

  fclose(a);
  fclose(b);
  rename(tmp, dst);

}


/* Case 4: tmin acceptance rules and analyze feedback must agree between the
   direct and -F execution paths across normal, crash, -x, exact, edges-only
   and -B configurations. */

int test_feedback_modes(const char *tmin, const char *analyze,
                        const char *target) {

  char *w = make_work();
  if (!w) return 50;

  char plain[256], crash[256], mask[256];
  path_join(plain, sizeof(plain), w, "plain");
  path_join(crash, sizeof(crash), w, "crash");
  path_join(mask, sizeof(mask), w, "mask");

  write_file(plain, "ABABABCD", 8);
  write_file(crash, "Kpadding", 8);

  /* An all-zero mask keeps every edge, so results stay deterministic. */
  int mfd = open(mask, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (mfd < 0) { remove_work(w); return 51; }
  for (int i = 0; i < 65536; i++) (void)!write(mfd, "\0", 1);
  close(mfd);

  const char *base_tail[] = { "-m", "none", "-t", "1000", "--", target,
                              "replay", NULL };

  /* Each scenario: input file, extra option words (NULL-terminated). */
  struct scenario {
    const char *input;
    const char *extra[6];
    const char *exact_env;
  };

  struct scenario scenarios[] = {
    { plain, { NULL }, NULL },
    { plain, { "-e", NULL }, NULL },
    { crash, { NULL }, NULL },
    { crash, { "-x", NULL }, NULL },
    { plain, { "-B", NULL, NULL }, NULL }, /* mask path inserted below */
  };

  for (unsigned s = 0; s < sizeof(scenarios) / sizeof(scenarios[0]); s++) {

    char od[256], of[256], cd[256], cf[256];
    snprintf(od, sizeof(od), "%s/o%u-d", w, s);
    snprintf(of, sizeof(of), "%s/o%u-f", w, s);
    snprintf(cd, sizeof(cd), "%s/c%u-d", w, s);
    snprintf(cf, sizeof(cf), "%s/c%u-f", w, s);

    const char *dargv[32];
    const char *fargv[32];
    int dk = 0, fk = 0;

    dargv[dk++] = tmin;
    fargv[fk++] = tmin;
    fargv[fk++] = "-F";

    dargv[dk] = "-i"; fargv[fk] = "-i"; dk++; fk++;
    dargv[dk] = scenarios[s].input; fargv[fk] = scenarios[s].input; dk++; fk++;
    dargv[dk] = "-o"; fargv[fk] = "-o"; dk++; fk++;
    dargv[dk] = od; fargv[fk] = of; dk++; fk++;

    for (int j = 0; scenarios[s].extra[j]; j++) {

      const char *opt = scenarios[s].extra[j];
      dargv[dk++] = opt;
      fargv[fk++] = opt;

      if (!strcmp(opt, "-B")) {

        dargv[dk++] = mask;
        fargv[fk++] = mask;

      }

    }

    for (unsigned j = 0; base_tail[j]; j++) {

      dargv[dk++] = base_tail[j];
      fargv[fk++] = base_tail[j];

    }

    dargv[dk] = NULL;
    fargv[fk] = NULL;

    if (run_capture(dargv, cd, NULL)) { remove_work(w); return 52 + s; }
    if (run_capture(fargv, cf, NULL)) { remove_work(w); return 60 + s; }
    if (!file_equals(od, of)) { remove_work(w); return 70 + s; }

  }

  /* AFL_TMIN_EXACT on a crashing input. */
  setenv("AFL_TMIN_EXACT", "1", 1);

  char ed[256], ef[256], ecd[256], ecf[256];
  path_join(ed, sizeof(ed), w, "ed");
  path_join(ef, sizeof(ef), w, "ef");
  path_join(ecd, sizeof(ecd), w, "ecd");
  path_join(ecf, sizeof(ecf), w, "ecf");

  const char *exd[] = {
    tmin, "-i", crash, "-o", ed, "-m", "none", "-t", "1000",
    "--", target, "replay", NULL
  };
  const char *exf[] = {
    tmin, "-F", "-i", crash, "-o", ef, "-m", "none", "-t", "1000",
    "--", target, "replay", NULL
  };

  int r1 = run_capture(exd, ecd, NULL);
  int r2 = run_capture(exf, ecf, NULL);
  unsetenv("AFL_TMIN_EXACT");

  if (r1 || r2 || !file_equals(ed, ef)) { remove_work(w); return 80; }

  /* analyze: normal and crashing inputs, normalized output must match. */
  const char *inputs[] = { plain, crash };

  for (unsigned s = 0; s < 2; s++) {

    char ad[256], af[256], acd[256], acf[256], nd[256], nf[256];
    snprintf(ad, sizeof(ad), "%s/a%u-d", w, s);
    snprintf(af, sizeof(af), "%s/a%u-f", w, s);
    snprintf(acd, sizeof(acd), "%s/ac%u-d", w, s);
    snprintf(acf, sizeof(acf), "%s/ac%u-f", w, s);
    snprintf(nd, sizeof(nd), "%s/n%u-d", w, s);
    snprintf(nf, sizeof(nf), "%s/n%u-f", w, s);

    const char *aa_d[] = {
      analyze, "-i", inputs[s], "-m", "none", "-t", "1000",
      "--", target, "replay", NULL
    };
    const char *aa_f[] = {
      analyze, "-F", "-i", inputs[s], "-m", "none", "-t", "1000",
      "--", target, "replay", NULL
    };

    if (run_capture(aa_d, acd, NULL)) { remove_work(w); return 82 + s; }
    if (run_capture(aa_f, acf, NULL)) { remove_work(w); return 84 + s; }

    normalize_copy(acd, nd);
    normalize_copy(acf, nf);

    if (!file_equals(nd, nf)) { remove_work(w); return 86 + s; }

  }

  remove_work(w);
  return 0;

}


/* Case 5: protocol and startup failures must be reported as failures, never
   disguised as ordinary target exits or silently retried via direct
   execution. */

int test_protocol_errors(const char *tmin, const char *analyze,
                         const char *target) {

  char *w = make_work();
  if (!w) return 90;

  char in[256], out[256], cap[256];
  path_join(in, sizeof(in), w, "in");
  path_join(out, sizeof(out), w, "out");
  path_join(cap, sizeof(cap), w, "cap");
  write_file(in, "ABCD", 4);

  const char *bad_modes[] = { "nopipe", "badhello", "badpid", "shortstatus" };

  for (unsigned m = 0; m < sizeof(bad_modes) / sizeof(bad_modes[0]); m++) {

    for (int tool = 0; tool < 2; tool++) {

      const char *bin = tool ? analyze : tmin;
      char cp[256];
      snprintf(cp, sizeof(cp), "%s/cap-%u-%d", w, m, tool);

      const char *argv[24];
      int k = 0;
      argv[k++] = bin;
      argv[k++] = "-F";
      argv[k++] = "-i";
      argv[k++] = in;
      if (!tool) { argv[k++] = "-o"; argv[k++] = out; }
      argv[k++] = "-m"; argv[k++] = "none";
      argv[k++] = "-t"; argv[k++] = "500";
      argv[k++] = "--"; argv[k++] = target;
      argv[k++] = bad_modes[m];
      argv[k] = NULL;

      int rc = run_capture(argv, cp, NULL);

      if (rc == 0) { remove_work(w); return 91 + m; }
      if (!file_contains(cp, "PROGRAM ABORT")) { remove_work(w); return 95 + m; }

      /* A failed run must not leave a minimized result behind. */
      if (!tool) {

        struct stat st;
        if (stat(out, &st) == 0) { remove_work(w); return 99 + m; }

      }

    }

  }

  /* A target binary that cannot be executed is an explicit failure too. */
  char miss_cap[256], miss_out[256];
  path_join(miss_cap, sizeof(miss_cap), w, "misscap");
  path_join(miss_out, sizeof(miss_out), w, "missout");

  const char *missing[] = {
    tmin, "-F", "-i", in, "-o", miss_out, "-m", "none", "-t", "500",
    "--", "/nonexistent/fst-target-bin", NULL
  };

  if (run_capture(missing, miss_cap, NULL) == 0) { remove_work(w); return 110; }
  if (!file_contains(miss_cap, "PROGRAM ABORT")) { remove_work(w); return 111; }

  remove_work(w);
  return 0;

}
