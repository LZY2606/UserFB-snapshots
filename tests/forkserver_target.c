/*
  Deterministic C/POSIX target for afl-tool-runner forkserver contracts.

  The same source supports:
    * ordinary forkserver children,
    * __AFL_LOOP persistent children,
    * explicit __AFL_DEFER_FORKSRV initialization,
    * stdin and named-file/@@ replay,
    * deterministic crashes, non-zero exits and hangs,
    * hand-written malformed forkserver protocol servers.

  It is deliberately linked against the real llvm_mode/afl-llvm-rt.o.c.
*/

#include "config.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>

extern u8 *__afl_area_ptr;
extern int __afl_persistent_loop(unsigned int max_cnt);
extern void __afl_manual_init(void);

#ifndef __AFL_LOOP
#define __AFL_LOOP(n) __afl_persistent_loop(n)
#endif

#define MAX_INPUT 8192

static u8 input_buf[MAX_INPUT];


static void log_event(const char *fmt, ...) {

  const char *path = getenv("FT_LOG");
  va_list ap;
  FILE *f;

  if (!path || !*path) return;

  f = fopen(path, "a");
  if (!f) return;

  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);

  fclose(f);

}


static void record_round(pid_t pid, u32 len, const u8 *data) {

  const char *path = getenv("FT_LOG");
  char *hex;
  u32 i;
  int fd;

  if (!path || !*path) return;

  hex = malloc((size_t)len * 2 + 2);
  if (!hex) return;

  for (i = 0; i < len; i++) {
    static const char digits[] = "0123456789abcdef";
    hex[i * 2] = digits[data[i] >> 4];
    hex[i * 2 + 1] = digits[data[i] & 15];
  }

  hex[len * 2] = 0;

  /* One O_APPEND write-ish stdio record per sequential tool round. */
  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd >= 0) {

    char prefix[128];
    int n = snprintf(prefix, sizeof(prefix),
                     "R pid=%ld len=%u payload=", (long)pid, len);
    if (write(fd, prefix, (size_t)n) >= 0) {
      if (len) (void)!write(fd, hex, (size_t)len * 2);
      else (void)!write(fd, "-", 1);
      (void)!write(fd, "\n", 1);
    }
    close(fd);

  }

  free(hex);

}


static u32 read_input(int argc, char **argv) {

  int fd = STDIN_FILENO;
  u32 len = 0;

  if (argc > 1) {

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      log_event("TARGET_OPEN_FAIL path=%s errno=%d\n", argv[1], errno);
      _exit(72);
    }

  }

  while (len < MAX_INPUT) {

    ssize_t n = read(fd, input_buf + len, MAX_INPUT - len);

    if (n > 0) {

      len += (u32)n;

    } else if (!n) {

      break;

    } else if (errno != EINTR) {

      if (fd != STDIN_FILENO) close(fd);
      _exit(73);

    }

  }

  if (fd != STDIN_FILENO) close(fd);
  return len;

}


static void consume_coverage(u32 len, const u8 *data) {

  const char *analysis_exit = getenv("FT_ANALYSIS_EXIT");
  u32 i;

  /* A fixed edge present in every execution. */
  __afl_area_ptr[10]++;

  if (analysis_exit) {

    /* Coverage is input-independent; changing the first byte makes all four
       analyze mutations exit abnormally, which afl-analyze folds into its
       checksum via the status xor. */
    if (len == 4 && data[0] != 'Q') _exit(9);
    return;

  }

  if (getenv("FT_SIMPLE")) {

    __afl_area_ptr[17]++;
    __afl_area_ptr[len && data[0] == 'A' ? 31 : 47]++;
    return;

  }

  /* Deterministic signal crash. Handle before generic byte coverage so EXACT
     minimization has a small, deterministic set of required edges. */
  if (len && data[0] == 'C') {

    __afl_area_ptr[70]++;
    if (len > 1 && data[1] == 'R') __afl_area_ptr[71]++;
    if (len > 2 && data[2] == 'X') __afl_area_ptr[72]++;
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(99);

  }

  /* Deterministic ordinary non-zero exit. tmin -x turns this into crash
     mode; afl-analyze always accounts for the bad status. */
  if (len && data[0] == 'E') {

    __afl_area_ptr[80]++;
    if (len > 1 && data[1] == 'X') __afl_area_ptr[81]++;
    _exit(7);

  }

  if (len) __afl_area_ptr[11]++;
  if (len && data[0] == 'A') __afl_area_ptr[12]++;
  if (len > 1 && data[1] == 'B') __afl_area_ptr[13]++;

  for (i = 0; i < len; i++)
    __afl_area_ptr[20 + (data[i] & 31)]++;

  /* Analyze turns the first byte of "AAAA" into '@' on one mutation. In the
     recovery contract that same deterministic candidate hangs instead. */
  if (len == 4 && data[0] == '@' && getenv("FT_TIMEOUT_CASE")) {
    for (;;) pause();
  }

  if (len == 4 && data[0] == '@') _exit(9);

}


static u32 read_full_frame(s32 fd, u32 *frame) {

  u8 *p = (u8 *)frame;
  u32 got = 0;

  while (got < 4) {

    ssize_t n = read(fd, p + got, 4 - got);

    if (n > 0) got += (u32)n;
    else if (!n) break;
    else if (errno != EINTR) _exit(140);

  }

  return got;

}


static s32 write_full_frame(s32 fd, const void *frame, size_t len) {

  const u8 *p = frame;
  size_t done = 0;

  while (done < len) {

    ssize_t n = write(fd, p + done, len - done);

    if (n > 0) done += (size_t)n;
    else if (errno != EINTR) return -1;

  }

  return 0;

}


/* Hand-written server used only by the protocol-error contract. The test
   sets __AFL_DEFER_FORKSRV so the real runtime constructor stands down. */
static int protocol_server(const char *mode) {

  u32 frame = 0;
  u32 hello = 0;

  if (!strcmp(mode, "handshake_eof")) {

    return 0;

  }

  if (!strcmp(mode, "handshake_sleep")) {

    sleep(10);
    return 0;

  }

  if (!strcmp(mode, "handshake_short")) {

    (void)!write(FORKSRV_FD + 1, "\0\0", 2);
    return 0;

  }

  if (write_full_frame(FORKSRV_FD + 1, &hello, 4) < 0) return 1;

  if (read_full_frame(FORKSRV_FD, &frame) != 4) return 1;

  if (!strcmp(mode, "pid_zero")) {

    frame = 0;
    (void)!write(FORKSRV_FD + 1, &frame, 4);
    for (;;) pause();

  }

  if (!strcmp(mode, "pid_negative")) {

    s32 bad = -1;
    (void)!write(FORKSRV_FD + 1, &bad, 4);
    for (;;) pause();

  }

  if (!strcmp(mode, "status_short")) {

    frame = (u32)getpid();
    (void)!write(FORKSRV_FD + 1, &frame, 4);
    (void)!write(FORKSRV_FD + 1, "\x07\x00", 2);
    return 0;

  }

  return 2;

}


int main(int argc, char **argv) {

  const char *proto = getenv("FT_PROTO");
  u32 len;

  if (proto) return protocol_server(proto);

  /* This is the expensive initialization the forkserver should perform
     once. The contract sets __AFL_DEFER_FORKSRV so this log line is emitted
     before the real runtime enters its server loop. */
  log_event("INIT pid=%ld ppid=%ld\n", (long)getpid(), (long)getppid());

  __afl_manual_init();

  while (__AFL_LOOP(1000000)) {

    pid_t pid = getpid();

    len = read_input(argc, argv);
    record_round(pid, len, input_buf);
    consume_coverage(len, input_buf);

  }

  return 0;

}
