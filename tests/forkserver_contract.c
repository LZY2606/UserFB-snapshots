/*
 * Real process/pipe/SHM contract tests for afl-tool-runner and the -F
 * modes of afl-tmin/afl-analyze.  The five externally declared test
 * functions are invoked by support/contract-main.c.
 */

#include "afl-tool-runner.h"
#include "../afl-tool-runner.c"

#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct tmin_pair {
  int status[2];
  char input[PATH_MAX];
  char out[2][PATH_MAX];
  char log[2][PATH_MAX];
  char target_log[2][PATH_MAX];
};

struct analyze_pair {
  int status[2];
  char out[2][PATH_MAX];
  char log[2][PATH_MAX];
};


static int removal_cb(const char *path, const struct stat *sb, int type,
                      struct FTW *ftw) {

  (void)sb;
  (void)type;
  (void)ftw;
  return remove(path);

}


static void cleanup_workdir(char *dir) {

  nftw(dir, removal_cb, 32, FTW_DEPTH | FTW_PHYS);

}


static int make_workdir(char *buf, size_t n) {

  const char *base = getenv("TMPDIR");

  if (!base || !*base) base = "/tmp";
  snprintf(buf, n, "%s/ft-contract.XXXXXX", base);
  return mkdtemp(buf) ? 0 : -1;

}


static void path_to(char *out, size_t n, const char *dir, const char *name) {

  snprintf(out, n, "%s/%s", dir, name);

}


static int write_bytes(const char *path, const void *data, size_t len,
                       int mode) {

  FILE *f = fopen(path, "wb");

  if (!f) return -1;
  if (len && fwrite(data, 1, len, f) != len) {
    fclose(f);
    return -1;
  }
  if (fclose(f)) return -1;
  if (mode >= 0 && chmod(path, (mode_t)mode) < 0) return -1;
  return 0;

}


static char *read_text(const char *path) {

  struct stat st;
  size_t len = 0;
  char *buf;
  FILE *f;

  if (stat(path, &st) || st.st_size < 0) return strdup("");

  f = fopen(path, "rb");
  if (!f) return strdup("");

  len = (size_t)st.st_size;
  buf = malloc(len + 1);
  if (!buf) {
    fclose(f);
    return strdup("");
  }

  if (len && fread(buf, 1, len, f) != len) {
    fclose(f);
    free(buf);
    return strdup("");
  }

  buf[len] = 0;
  fclose(f);
  return buf;

}


static int files_equal(const char *a, const char *b) {

  struct stat sa, sb;
  char *ba, *bb;
  int equal;

  if (stat(a, &sa) || stat(b, &sb)) return 0;
  if (sa.st_size != sb.st_size) return 0;
  if (!sa.st_size) return 1;

  ba = read_text(a);
  bb = read_text(b);
  equal = ba && bb && !memcmp(ba, bb, (size_t)sa.st_size);
  free(ba);
  free(bb);
  return equal;

}


static int tool_succeeded(int status) {

  return WIFEXITED(status) && WEXITSTATUS(status) == 0;

}


static int run_capture(const char *cwd, const char *exe,
                       char *const argv[], const char *out_path) {

  pid_t pid = fork();

  if (pid < 0) return -1;

  if (!pid) {

    int fd, nullfd;

    if (cwd && chdir(cwd) < 0) _exit(126);

    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) close(fd);
    }

    nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) {
      dup2(nullfd, STDIN_FILENO);
      if (nullfd > STDERR_FILENO) close(nullfd);
    }

    execv(exe, argv);
    _exit(127);

  }

  int status = 0;

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }

  return status;

}


static void set_common_env(const char *dir, const char *log) {

  char shm[PATH_MAX];

  path_to(shm, sizeof(shm), dir, "shm");
  mkdir(shm, 0700);
  setenv("SHM_ROOT", shm, 1);
  setenv("__AFL_DEFER_FORKSRV", "1", 1);
  if (log) setenv("FT_LOG", log, 1);
  unsetenv("FT_PROTO");
  unsetenv("FT_TIMEOUT_CASE");
  unsetenv("FT_ANALYSIS_EXIT");
  unsetenv("FT_SIMPLE");

}


/* Return concatenated " len=... payload=..." portions of R records. This
   compares the bytes the target observed without comparing PIDs. */
static char *round_suffixes(const char *log_path) {

  char *text = read_text(log_path);
  size_t cap = strlen(text) + 1;
  char *out = calloc(1, cap);
  char *p = text;

  if (!out) {
    free(text);
    return strdup("");
  }

  while (p && *p) {

    char *eol = strchr(p, '\n');
    char saved = 0;
    char *lenp;

    if (eol) {
      saved = *eol;
      *eol = 0;
    }

    if (!strncmp(p, "R pid=", 6) && (lenp = strstr(p, " len="))) {
      size_t used = strlen(out);
      snprintf(out + used, cap - used, "%s\n", lenp);
    }

    if (eol) {
      *eol = saved;
      p = eol + 1;
    } else break;

  }

  free(text);
  return out;

}


static int count_init_lines(const char *log_path) {

  char *text = read_text(log_path);
  int count = 0;
  char *p = text;

  while ((p = strstr(p, "\nINIT ")) != NULL) {
    count++;
    p++;
  }

  if (!strncmp(text, "INIT ", 5)) count++;
  free(text);
  return count;

}


static size_t get_round_pids(const char *log_path, long *pids,
                             size_t max_pids) {

  char *text = read_text(log_path);
  char *line = text;
  size_t n = 0;

  while (line && *line && n < max_pids) {

    char *eol = strchr(line, '\n');
    long pid;
    unsigned len;

    if (eol) *eol = 0;

    if (sscanf(line, "R pid=%ld len=%u", &pid, &len) == 2)
      pids[n++] = pid;

    if (!eol) break;
    line = eol + 1;

  }

  free(text);
  return n;

}


static int run_direct_sequence(const char *target, const char *dir,
                               const char *log, int file_mode,
                               const char *expected) {

  static u8 trace[MAP_SIZE];
  volatile s32 child_pid = 0;
  volatile u8 timed_out = 0, stop_soon = 0;
  char candidate[PATH_MAX], log_path[PATH_MAX];
  char *target_argv[3];
  struct afl_tool_runner_options opts;
  afl_tool_runner *runner;
  int devnull, rc = 1;

  path_to(candidate, sizeof(candidate), dir, "sequence-input");
  path_to(log_path, sizeof(log_path), dir, log);
  unlink(candidate);

  set_common_env(dir, log_path);
  setenv("FT_LOG", log_path, 1);

  target_argv[0] = (char *)target;
  target_argv[1] = file_mode ? candidate : NULL;
  target_argv[2] = NULL;

  devnull = open("/dev/null", O_RDWR);
  if (devnull < 0) return 20;

  memset(&opts, 0, sizeof(opts));
  opts.mode = AFL_TOOL_RUNNER_FORKSRV;
  opts.target_path = (const u8 *)target;
  opts.argv = target_argv;
  opts.prog_in = (const u8 *)candidate;
  opts.trace_bits = trace;
  opts.dev_null_fd = devnull;
  opts.timeout_ms = 2000;
  opts.mem_limit_mb = 0;
  opts.use_stdin = !file_mode;
  opts.new_session = 1;
  opts.child_pid = &child_pid;
  opts.timed_out = &timed_out;
  opts.stop_soon = &stop_soon;

  runner = afl_tool_runner_create(&opts);
  if (afl_tool_runner_start(runner) < 0) {
    fprintf(stderr, "sequence start failed: %s\n",
            afl_tool_runner_error(runner));
    goto out;
  }

  struct candidate { const char *data; u32 len; };
  const struct candidate cases[] = {
    { "ABCD", 4 }, { "ABCD", 4 }, { "ABC", 3 },
    { "", 0 }, { "XY", 2 }
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {

    int status;

    if (afl_tool_runner_run(runner, (const u8 *)cases[i].data,
                            cases[i].len, &status) < 0) {
      fprintf(stderr, "sequence run %zu failed: %s\n", i,
              afl_tool_runner_error(runner));
      goto out;
    }

    if (!WIFSTOPPED(status)) {
      fprintf(stderr, "sequence run %zu did not stop at persistent boundary "
                      "(status=0x%x)\n", i, status);
      goto out;
    }

  }

  afl_tool_runner_destroy(&runner);
  unsetenv(PERSIST_ENV_VAR);
  close(devnull);

  char *observed = round_suffixes(log_path);
  if (strcmp(observed, expected)) {
    fprintf(stderr, "%s replay mismatch\nobserved: %s\nexpected: %s\n",
            file_mode ? "file" : "stdin", observed, expected);
    free(observed);
    return 21;
  }
  free(observed);

  long pids[16];
  size_t npids = get_round_pids(log_path, pids, 16);
  if (npids != 5) {
    fprintf(stderr, "%s sequence logged %zu rounds, expected 5\n",
            file_mode ? "file" : "stdin", npids);
    return 22;
  }
  for (size_t i = 1; i < npids; i++) {
    if (pids[i] != pids[0]) {
      fprintf(stderr, "%s persistent PID changed from %ld to %ld\n",
              file_mode ? "file" : "stdin", pids[0], pids[i]);
      return 23;
    }
  }

  rc = 0;

out:
  if (runner) afl_tool_runner_destroy(&runner);
  unsetenv(PERSIST_ENV_VAR);
  if (devnull >= 0) close(devnull);
  return rc;

}


static int run_tmin_pair(const char *tmin, const char *target,
                         const char *dir, const char *tag,
                         const char *input, u32 input_len,
                         char *const *extra_args,
                         char *const *target_args, int exact_mode,
                         struct tmin_pair *result) {

  int argn = 0;

  memset(result, 0, sizeof(*result));
  path_to(result->input, sizeof(result->input), dir, tag);
  write_bytes(result->input, input, input_len, -1);

  if (exact_mode) setenv("AFL_TMIN_EXACT", "1", 1);
  else unsetenv("AFL_TMIN_EXACT");

  for (int fork_mode = 0; fork_mode < 2; fork_mode++) {

    char *args[64];

    snprintf(result->out[fork_mode], PATH_MAX, "%s/%s-%s.min", dir, tag,
             fork_mode ? "f" : "d");
    snprintf(result->log[fork_mode], PATH_MAX, "%s/%s-%s.tool.log", dir, tag,
             fork_mode ? "f" : "d");
    snprintf(result->target_log[fork_mode], PATH_MAX,
             "%s/%s-%s.target.log", dir, tag, fork_mode ? "f" : "d");
    setenv("FT_LOG", result->target_log[fork_mode], 1);

    argn = 0;
    args[argn++] = (char *)tmin;
    if (fork_mode) args[argn++] = "-F";
    args[argn++] = "-i";
    args[argn++] = result->input;
    args[argn++] = "-o";
    args[argn++] = result->out[fork_mode];
    args[argn++] = "-m";
    args[argn++] = "none";
    args[argn++] = "-t";
    args[argn++] = "1000";

    if (extra_args)
      for (char *const *e = extra_args; *e; e++)
        args[argn++] = *e;

    args[argn++] = "--";
    args[argn++] = (char *)target;

    if (target_args)
      for (char *const *a = target_args; *a; a++)
        args[argn++] = *a;

    args[argn++] = NULL;

    result->status[fork_mode] =
      run_capture(dir, tmin, args, result->log[fork_mode]);

  }

  unsetenv("AFL_TMIN_EXACT");
  return tool_succeeded(result->status[0]) &&
         tool_succeeded(result->status[1]) &&
         files_equal(result->out[0], result->out[1]);

}


static int run_analyze_pair(const char *analyze, const char *target,
                            const char *dir, const char *tag,
                            const char *input, u32 input_len,
                            char *const *extra_args,
                            struct analyze_pair *result) {

  memset(result, 0, sizeof(*result));
  char input_path[PATH_MAX];

  path_to(input_path, sizeof(input_path), dir, tag);
  write_bytes(input_path, input, input_len, -1);

  for (int fork_mode = 0; fork_mode < 2; fork_mode++) {

    char *args[64];
    int n = 0;

    snprintf(result->out[fork_mode], PATH_MAX, "%s/%s-%s.out", dir, tag,
             fork_mode ? "f" : "d");
    snprintf(result->log[fork_mode], PATH_MAX, "%s/%s-%s.target", dir, tag,
             fork_mode ? "f" : "d");
    setenv("FT_LOG", result->log[fork_mode], 1);

    args[n++] = (char *)analyze;
    if (fork_mode) args[n++] = "-F";
    args[n++] = "-i";
    args[n++] = input_path;
    args[n++] = "-m";
    args[n++] = "none";
    args[n++] = "-t";
    args[n++] = "1000";

    if (extra_args)
      for (char *const *e = extra_args; *e; e++) args[n++] = *e;

    args[n++] = "--";
    args[n++] = (char *)target;
    args[n++] = NULL;

    result->status[fork_mode] =
      run_capture(dir, analyze, args, result->out[fork_mode]);

  }

  return tool_succeeded(result->status[0]) &&
         tool_succeeded(result->status[1]) &&
         files_equal(result->out[0], result->out[1]);

}


int test_stdin_replay(const char *tmin, const char *analyze,
                      const char *target) {

  char dir[PATH_MAX], in[PATH_MAX], logd[PATH_MAX], logf[PATH_MAX];
  struct tmin_pair pair;
  char *rounds_d, *rounds_f;
  int rc = 1;

  (void)analyze;

  if (make_workdir(dir, sizeof(dir))) return 1;
  set_common_env(dir, NULL);

  static const char expected[] =
    " len=4 payload=41424344\n"
    " len=4 payload=41424344\n"
    " len=3 payload=414243\n"
    " len=0 payload=-\n"
    " len=2 payload=5859\n";

  if (run_direct_sequence(target, dir, "sequence-stdin.log", 0, expected))
    goto out;

  setenv("FT_SIMPLE", "1", 1);

  path_to(in, sizeof(in), dir, "stdin-in");
  path_to(logd, sizeof(logd), dir, "stdin-d.log");
  path_to(logf, sizeof(logf), dir, "stdin-f.log");
  write_bytes(in, "Apadding", 8, -1);

  char *extra[] = { NULL };
  if (!run_tmin_pair(tmin, target, dir, "stdin-tmin", "Apadding", 8,
                     extra, NULL, 0, &pair)) {
    char *out = read_text(pair.log[1]);
    fprintf(stderr, "stdin tmin pair failed\n%s\n", out);
    free(out);
    goto out;
  }

  if (!files_equal(pair.out[0], pair.out[1])) goto out;
  char *mind = read_text(pair.out[0]);
  if (strcmp(mind, "A")) {
    fprintf(stderr, "stdin min is not A: '%s'\n", mind);
    free(mind);
    goto out;
  }
  free(mind);

  /* run_tmin_pair records target payloads in target_log. */
  rounds_d = round_suffixes(pair.target_log[0]);
  rounds_f = round_suffixes(pair.target_log[1]);
  if (strcmp(rounds_d, rounds_f)) {
    fprintf(stderr, "stdin direct/F candidate replay differs\nD:%s\nF:%s\n",
            rounds_d, rounds_f);
    free(rounds_d);
    free(rounds_f);
    goto out;
  }
  free(rounds_d);
  free(rounds_f);

  if (count_init_lines(pair.target_log[1]) != 1 ||
      count_init_lines(pair.target_log[0]) < 2) {
    fprintf(stderr, "stdin initialization reuse count is wrong\n");
    goto out;
  }

  rc = 0;

out:
  unsetenv("FT_SIMPLE");
  cleanup_workdir(dir);
  return rc;

}


int test_file_replay(const char *tmin, const char *analyze,
                     const char *target) {

  char dir[PATH_MAX], forced[PATH_MAX];
  struct tmin_pair at_pair, f_pair;
  int rc = 1;

  (void)analyze;

  if (make_workdir(dir, sizeof(dir))) return 1;
  set_common_env(dir, NULL);

  static const char expected[] =
    " len=4 payload=41424344\n"
    " len=4 payload=41424344\n"
    " len=3 payload=414243\n"
    " len=0 payload=-\n"
    " len=2 payload=5859\n";

  if (run_direct_sequence(target, dir, "sequence-file.log", 1, expected))
    goto out;

  setenv("FT_SIMPLE", "1", 1);

  /* @@ is a target argument, not a tool option. */
  char *at_options[] = { NULL };
  char *at_target_args[] = { "@@", NULL };
  if (!run_tmin_pair(tmin, target, dir, "at-file", "Apadding", 8,
                     at_options, at_target_args, 0, &at_pair)) {
    char *out = read_text(at_pair.log[1]);
    fprintf(stderr, "@@ replay pair failed\n%s\n", out);
    free(out);
    goto out;
  }

  if (!files_equal(at_pair.out[0], at_pair.out[1])) goto out;
  char *rd = round_suffixes(at_pair.target_log[0]);
  char *rf = round_suffixes(at_pair.target_log[1]);
  if (strcmp(rd, rf)) {
    fprintf(stderr, "@@ replay differs\nD:%s\nF:%s\n", rd, rf);
    free(rd);
    free(rf);
    goto out;
  }
  free(rd);
  free(rf);

  /* Explicit -f path plus the same path as the target's file argument. */
  path_to(forced, sizeof(forced), dir, "forced-input");
  char *f_options[] = { "-f", forced, NULL };
  char *f_target_args[] = { forced, NULL };
  if (!run_tmin_pair(tmin, target, dir, "f-file", "Apadding", 8,
                     f_options, f_target_args, 0, &f_pair)) {
    char *out = read_text(f_pair.log[1]);
    fprintf(stderr, "-f replay pair failed\n%s\n", out);
    free(out);
    goto out;
  }

  rd = round_suffixes(f_pair.target_log[0]);
  rf = round_suffixes(f_pair.target_log[1]);
  if (strcmp(rd, rf) || !files_equal(f_pair.out[0], f_pair.out[1])) {
    fprintf(stderr, "-f replay differs\nD:%s\nF:%s\n", rd, rf);
    free(rd);
    free(rf);
    goto out;
  }
  free(rd);
  free(rf);

  rc = 0;

out:
  unsetenv("FT_SIMPLE");
  cleanup_workdir(dir);
  return rc;

}


int test_persistent_recovery(const char *tmin, const char *analyze,
                             const char *target) {

  char dir[PATH_MAX], in[PATH_MAX], out[PATH_MAX], tlog[PATH_MAX];
  char *output = NULL;
  long pids[256];
  size_t npids;
  int rc = 1, status;

  (void)tmin;

  if (make_workdir(dir, sizeof(dir))) return 1;
  set_common_env(dir, tlog);
  setenv("FT_TIMEOUT_CASE", "1", 1);

  path_to(in, sizeof(in), dir, "timeout-in");
  path_to(out, sizeof(out), dir, "analyze-timeout.out");
  path_to(tlog, sizeof(tlog), dir, "analyze-timeout.target");
  write_bytes(in, "AAAA", 4, -1);
  setenv("FT_LOG", tlog, 1);

  char *args[] = {
    (char *)analyze, "-F", "-i", in, "-m", "none", "-t", "200",
    "--", (char *)target, NULL
  };

  status = run_capture(dir, analyze, args, out);
  output = read_text(out);

  if (!tool_succeeded(status)) {
    fprintf(stderr, "persistent recovery analyze failed:\n%s\n", output);
    goto out;
  }

  if (!strstr(output, "Encountered 1 timeouts") ||
      !strstr(output, "Analysis complete")) {
    fprintf(stderr, "timeout was not counted/recovered:\n%s\n", output);
    goto out;
  }

  npids = get_round_pids(tlog, pids, 256);
  if (npids < 10) {
    fprintf(stderr, "only %zu target rounds after timeout\n", npids);
    goto out;
  }

  if (pids[0] != pids[1]) {
    fprintf(stderr, "persistent PID was not reused before timeout: "
                    "%ld != %ld\n", pids[0], pids[1]);
    goto out;
  }

  int saw_replacement = 0;
  for (size_t i = 2; i < npids; i++)
    if (pids[i] != pids[0]) saw_replacement = 1;

  if (!saw_replacement) {
    fprintf(stderr, "timeout did not replace killed persistent child\n");
    goto out;
  }

  if (count_init_lines(tlog) != 1) {
    fprintf(stderr, "forkserver initialization count != 1\n");
    goto out;
  }

  rc = 0;

out:
  free(output);
  unsetenv("FT_TIMEOUT_CASE");
  cleanup_workdir(dir);
  return rc;

}


static int check_simple_tmin(struct tmin_pair *pair, const char *expected) {

  char *min;

  if (!tool_succeeded(pair->status[0]) || !tool_succeeded(pair->status[1]))
    return 1;

  min = read_text(pair->out[0]);
  if (strcmp(min, expected) || !files_equal(pair->out[0], pair->out[1])) {
    fprintf(stderr, "expected min '%s', got '%s'\n", expected, min);
    free(min);
    return 1;
  }
  free(min);
  return 0;

}


int test_feedback_modes(const char *tmin, const char *analyze,
                        const char *target) {

  char dir[PATH_MAX], mask[PATH_MAX];
  struct tmin_pair normal, edges, masked, exitpair, exactpair, crashextra;
  struct analyze_pair apair;
  char mask_bits[MAP_SIZE];
  char *analysis;
  int rc = 1;

  if (make_workdir(dir, sizeof(dir))) return 1;
  set_common_env(dir, NULL);
  setenv("FT_SIMPLE", "1", 1);

  char *no_extra[] = { NULL };
  if (!run_tmin_pair(tmin, target, dir, "normal", "Apadding", 8,
                     no_extra, NULL, 0, &normal) ||
      check_simple_tmin(&normal, "A")) {
    char *log = read_text(normal.log[1]);
    fprintf(stderr, "normal tmin feedback failed\n%s\n", log);
    free(log);
    goto out;
  }

  char *edge_extra[] = { "-e", NULL };
  if (!run_tmin_pair(tmin, target, dir, "edges", "Apadding", 8,
                     edge_extra, NULL, 0, &edges) ||
      check_simple_tmin(&edges, "A")) {
    fprintf(stderr, "-e feedback differs from direct mode\n");
    goto out;
  }

  memset(mask_bits, 0, sizeof(mask_bits));
  mask_bits[17] = (char)0xff;
  mask_bits[31] = (char)0xff;
  mask_bits[47] = (char)0xff;
  path_to(mask, sizeof(mask), dir, "mask.bin");
  write_bytes(mask, mask_bits, sizeof(mask_bits), -1);

  char *mask_extra[] = { "-B", mask, NULL };
  if (!run_tmin_pair(tmin, target, dir, "masked", "Apadding", 8,
                     mask_extra, NULL, 0, &masked)) {
    fprintf(stderr, "-B direct/F pair failed\n");
    goto out;
  }
  if (stat(masked.out[0], &(struct stat){0}) ||
      stat(masked.out[1], &(struct stat){0})) {
    goto out;
  }
  if (!files_equal(masked.out[0], masked.out[1])) goto out;
  if (files_equal(masked.out[0], normal.out[0]) ||
      strlen(read_text(masked.out[0])) != 0) {
    fprintf(stderr, "-B did not change acceptance to empty output\n");
    goto out;
  }

  unsetenv("FT_SIMPLE");

  char *x_extra[] = { "-x", NULL };
  if (!run_tmin_pair(tmin, target, dir, "exit-x", "EXITxxxx", 8,
                     x_extra, NULL, 0, &exitpair) ||
      check_simple_tmin(&exitpair, "E")) {
    char *log = read_text(exitpair.log[1]);
    fprintf(stderr, "-x feedback failed\n%s\n", log);
    free(log);
    goto out;
  }

  if (!run_tmin_pair(tmin, target, dir, "crash-plain", "CRXpadding", 10,
                     no_extra, NULL, 0, &crashextra) ||
      check_simple_tmin(&crashextra, "C")) {
    fprintf(stderr, "plain crash minimization expected C\n");
    goto out;
  }

  if (!run_tmin_pair(tmin, target, dir, "crash-exact", "CRXpadding", 10,
                     no_extra, NULL, 1, &exactpair)) {
    fprintf(stderr, "AFL_TMIN_EXACT direct/F pair failed\n");
    goto out;
  }
  char *exact_min = read_text(exactpair.out[0]);
  if (memcmp(exact_min, "CRX", 3) ||
      !files_equal(exactpair.out[0], exactpair.out[1])) {
    fprintf(stderr, "EXACT crash min should retain CRX, got '%s'\n",
            exact_min);
    free(exact_min);
    goto out;
  }
  free(exact_min);

  setenv("FT_ANALYSIS_EXIT", "1", 1);
  if (!run_analyze_pair(analyze, target, dir, "analyze-exit",
                        "QQQQ", 4, no_extra, &apair)) {
    char *log = read_text(apair.out[1]);
    fprintf(stderr, "analyze feedback pair failed status=%d/%d\n%s\n",
            apair.status[0], apair.status[1], log);
    free(log);
    goto out;
  }

  analysis = read_text(apair.out[1]);
  if (!strstr(analysis, "Interesting bits: 25.00%") ||
      !strstr(analysis, "Analysis complete")) {
    fprintf(stderr, "abnormal exits not counted by analyze:\n%s\n",
            analysis);
    free(analysis);
    goto out;
  }
  free(analysis);

  rc = 0;

out:
  unsetenv("FT_SIMPLE");
  unsetenv("FT_ANALYSIS_EXIT");
  unsetenv("AFL_TMIN_EXACT");
  cleanup_workdir(dir);
  return rc;

}


static int expect_failure(const char *exe, const char *target,
                          const char *dir, const char *mode,
                          const char *needle, int timeout_ms, int analyze) {

  char in[PATH_MAX], out[PATH_MAX];
  char *text;
  int status;

  path_to(in, sizeof(in), dir, "proto-in");
  path_to(out, sizeof(out), dir, mode ? mode : "bad-exec-out");
  write_bytes(in, "proto", 5, -1);

  if (mode) setenv("FT_PROTO", mode, 1);
  else unsetenv("FT_PROTO");

  char *args[16];
  int n = 0;
  args[n++] = (char *)exe;
  args[n++] = "-F";
  args[n++] = "-i";
  args[n++] = in;
  if (!analyze) {
    args[n++] = "-o";
    args[n++] = out;
  }
  args[n++] = "-m";
  args[n++] = "none";
  args[n++] = "-t";
  char tbuf[32];
  snprintf(tbuf, sizeof(tbuf), "%d", timeout_ms);
  args[n++] = tbuf;
  args[n++] = "--";
  args[n++] = (char *)target;
  args[n++] = NULL;

  status = run_capture(dir, exe, args, out);
  text = read_text(out);

  if (tool_succeeded(status)) {
    fprintf(stderr, "%s: protocol failure unexpectedly succeeded\n%s\n",
            mode ? mode : "exec", text);
    free(text);
    return 1;
  }

  if (needle && !strstr(text, needle)) {
    fprintf(stderr, "%s: output lacks '%s'\n%s\n",
            mode ? mode : "exec", needle, text);
    free(text);
    return 1;
  }

  free(text);
  return 0;

}


int test_protocol_errors(const char *tmin, const char *analyze,
                         const char *target) {

  char dir[PATH_MAX], bad_target[PATH_MAX];
  struct { const char *mode; const char *needle; int timeout; } cases[] = {
    { "handshake_eof", "closed the status pipe before handshake", 20 },
    { "handshake_short", "incomplete forkserver handshake frame", 20 },
    { "pid_zero", "invalid child PID 0", 20 },
    { "pid_negative", "invalid child PID -1", 20 },
    { "status_short", "incomplete forkserver status frame", 20 },
    { "handshake_sleep", "timeout while initializing fork server", 10 }
  };
  int rc = 1;

  if (make_workdir(dir, sizeof(dir))) return 1;
  set_common_env(dir, NULL);

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {

    if (expect_failure(tmin, target, dir, cases[i].mode,
                       cases[i].needle, cases[i].timeout, 0))
      goto out;

  }

  /* The shared backend is used by analyze too; check one malformed PID. */
  if (expect_failure(analyze, target, dir, "pid_zero",
                     "invalid child PID 0", 20, 1))
    goto out;

  path_to(bad_target, sizeof(bad_target), dir, "bad-exec");
  if (write_bytes(bad_target, "this is not an executable\n", 26, 0755))
    goto out;

  if (expect_failure(tmin, bad_target, dir, NULL,
                     "Unable to execute target application", 1000, 0))
    goto out;

  rc = 0;

out:
  unsetenv("FT_PROTO");
  cleanup_workdir(dir);
  return rc;

}
