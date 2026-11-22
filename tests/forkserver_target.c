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
   Deterministic local target for the afl-tmin / afl-analyze forkserver
   contract tests. It is linked against the real llvm_mode runtime and drives
   it through the same pipes and bitmap protocol as afl-fuzz.

   Modes (selected on the command line):

     replay [file]      one shot per process; reads stdin or [file] to EOF
     persist [file]     __AFL_LOOP() worker, one iteration per candidate
     defer   [file]     explicit __afl_manual_init(), then replay semantics

   Every process initialization and every executed candidate is appended to
   the file named by FST_LOG. The markers below make crashes, nonzero exits
   and hangs deterministic:

     input containing 'K'  -> SIGSEGV
     input containing 'X'  -> exit status 3
     input equal to HANG   -> never returns (timeout recovery tests)

   The PERSIST_SIG / DEFER_SIG byte patterns are emitted as real strings so
   that the tool-side signature scan (the same scan afl-fuzz performs) finds
   them in the compiled binary.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "../config.h"

extern unsigned char *__afl_area_ptr;
extern int __afl_persistent_loop(unsigned int max_cnt);
extern void __afl_manual_init(void);

/* Keep the signatures in the binary even under optimization. */

static const char persist_sig[] __attribute__((used)) = PERSIST_SIG;
static const char defer_sig[]   __attribute__((used)) = DEFER_SIG;

#define MAX_CANDIDATE 4096

static const char *log_path;

/* Append one line to the shared event log. Lines are small and the tool runs
   candidates sequentially, so O_APPEND keeps them ordered and intact. */

static void log_line(const char *kind, unsigned iter, const unsigned char *buf,
                     size_t len) {

  if (!log_path) return;

  int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) _exit(70);

  char header[128];
  int hn = snprintf(header, sizeof(header),
                    "%s pid=%ld iter=%u len=%zu bytes=", kind,
                    (long)getpid(), iter, len);
  if (hn > 0 && write(fd, header, (size_t)hn) != hn) _exit(71);

  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {

    char pair[2] = { hex[buf[i] >> 4], hex[buf[i] & 15] };
    if (write(fd, pair, 2) != 2) _exit(72);

  }

  if (write(fd, "\n", 1) != 1) _exit(73);
  close(fd);

}

/* Runs once per freshly executed process, inside the runtime constructor. */

__attribute__((constructor)) static void record_init(void) {

  log_path = getenv("FST_LOG");
  log_line("INIT", 0, (const unsigned char *)"", 0);

}


static size_t read_all(int fd, unsigned char *buf, size_t cap) {

  size_t used = 0;

  while (used < cap) {

    ssize_t n = read(fd, buf + used, cap - used);

    if (n < 0) {

      if (errno == EINTR) continue;
      _exit(74);

    }
    if (!n) break;
    used += (size_t)n;

  }

  return used;

}


/* Deterministic, content-dependent coverage and exit behavior. */

static void consume(const unsigned char *buf, size_t len, unsigned iter) {

  __afl_area_ptr[17]++;

  if (len && buf[0] == 'A') __afl_area_ptr[31]++;
  if (len >= 2 && buf[1] == 'B') __afl_area_ptr[47]++;
  if (len >= 4) __afl_area_ptr[63 + (len & 7)]++;

  for (size_t i = 0; i < len; i++)
    __afl_area_ptr[96 + (buf[i] & 31)]++;

  log_line("RUN", iter, buf, len);

  if (len == 4 && !memcmp(buf, "HANG", 4)) {

    for (;;) pause();

  }

  /* A 'Z' byte hangs the worker. afl-analyze's first mutation of an 0xA5
     byte is xor 0xff -> 0x5a ('Z'), giving a deterministic single timeout. */
  for (size_t i = 0; i < len; i++)
    if (buf[i] == 'Z') for (;;) pause();

  for (size_t i = 0; i < len; i++) {

    if (buf[i] == 'K') raise(SIGSEGV);
    if (buf[i] == 'X') _exit(3);

  }

}


/* Prefix-only coverage: the path depends solely on the leading byte, so a
   long input can be deterministically minimized while every per-run byte
   count (and thus EOF behavior) is still recorded. The crash/exit markers
   keep working for cross-checks. */

static void consume_prefix(const unsigned char *buf, size_t len,
                           unsigned iter) {

  __afl_area_ptr[17]++;
  if (len && buf[0] == 'A') __afl_area_ptr[31]++;

  log_line("RUN", iter, buf, len);

  for (size_t i = 0; i < len; i++) {

    if (buf[i] == 'K') raise(SIGSEGV);
    if (buf[i] == 'X') _exit(3);

  }

}


int main(int argc, char **argv) {

  const char *mode = argc > 1 ? argv[1] : "replay";
  unsigned char buf[MAX_CANDIDATE];
  unsigned iter = 0;

  /* Misbehaving-forkserver modes. The binary carries the deferred signature,
     so the tool exports __AFL_DEFER_FORKSRV and the runtime constructor never
     touches the control/status pipes; main() then speaks a deliberately
     broken protocol on FORKSRV_FD / FORKSRV_FD + 1 itself. These exist for
     the protocol-error contract case and always terminate on their own. */

  if (!strcmp(mode, "nopipe")) {

    /* Handshake EOF: no hello frame, just exit. */
    return 0;

  }

  if (!strcmp(mode, "badhello")) {

    /* Truncated hello frame (two of four bytes). */
    if (write(FORKSRV_FD + 1, "ab", 2) != 2) _exit(76);
    return 0;

  }

  if (!strcmp(mode, "badpid") || !strcmp(mode, "shortstatus")) {

    u32 hello = 0;
    u32 cmd;

    if (write(FORKSRV_FD + 1, &hello, 4) != 4) _exit(77);
    if (read(FORKSRV_FD, &cmd, 4) != 4) _exit(78);

    if (!strcmp(mode, "badpid")) {

      /* Valid handshake, then an invalid (zero) child PID. */
      u32 bad = 0;
      if (write(FORKSRV_FD + 1, &bad, 4) != 4) _exit(79);
      return 0;

    }

    /* Valid PID, then a truncated status frame. */
    u32 pid = (u32)getpid();
    if (write(FORKSRV_FD + 1, &pid, 4) != 4) _exit(80);
    if (write(FORKSRV_FD + 1, "ab", 2) != 2) _exit(81);
    return 0;

  }

  /* The binary carries both mode signatures, so the tool may export both
     __AFL_PERSISTENT and __AFL_DEFER_FORKSRV. In deferred mode the runtime
     constructor skips initialization; start the forkserver explicitly here,
     before any mode-specific work. __afl_manual_init() is idempotent. */

  if (getenv(DEFER_ENV_VAR) || !strcmp(mode, "defer")) {

    log_line("MANUAL_INIT", 0, (const unsigned char *)"", 0);
    __afl_manual_init();

  }

  if (!strcmp(mode, "persist") || !strcmp(mode, "pref")) {

    while (__afl_persistent_loop(1000000)) {

      int fd = 0;
      int opened = 0;

      iter++;

      if (argc > 2) {

        fd = open(argv[2], O_RDONLY);
        if (fd < 0) _exit(75);
        opened = 1;

      }

      size_t len = read_all(fd, buf, sizeof(buf));
      if (opened) close(fd);

      if (!strcmp(mode, "pref"))
        consume_prefix(buf, len, iter);
      else
        consume(buf, len, iter);

    }

    return 0;

  }

  /* replay, defer and crashall modes: a single candidate per process. */

  int fd = 0;
  int opened = 0;

  if (argc > 2) {

    fd = open(argv[2], O_RDONLY);
    if (fd < 0) _exit(75);
    opened = 1;

  }

  size_t len = read_all(fd, buf, sizeof(buf));
  if (opened) close(fd);

  if (!strcmp(mode, "crashall")) {

    /* Crashes on every candidate, including the empty one, so that afl-tmin
       in crash mode deterministically exercises a zero-length replay. */

    log_line("RUN", 1, buf, len);
    raise(SIGSEGV);

  }

  consume(buf, len, 1);

  return 0;

}
