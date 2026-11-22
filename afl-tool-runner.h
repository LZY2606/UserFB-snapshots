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
   american fuzzy lop - shared target execution backend
   -----------------------------------------------------

   afl-tmin and afl-analyze historically re-executed the target for every
   candidate. When the target is built with the LLVM instrumentation runtime,
   it already contains a forkserver (optionally with a persistent loop or a
   deferred __afl_manual_init() entry point). This module drives that
   forkserver directly, so the target's initialization cost is paid once per
   tool process.

   The wire protocol is the one implemented by llvm_mode/afl-llvm-rt.o.c and
   consumed by afl-fuzz:

     - the tool creates two pipes and dup2()s them onto FORKSRV_FD and
       FORKSRV_FD + 1 before execv()'ing the target;
     - the runtime writes a four-byte hello frame once the forkserver is up;
     - per execution the tool writes a four-byte control word and reads back
       a four-byte child PID, then a four-byte raw wait(2) status;
     - a persistent child announces the end of a loop iteration with
       SIGSTOP (reported as a stopped status), which is a normal completion,
       not a crash; the same process is then resumed with SIGCONT;
     - the control word is the previous-timeout flag, exactly as in afl-fuzz.

   The shared coverage bitmap and its layout are untouched: the tool keeps
   owning the SHM region identified by SHM_ENV_VAR.

   Without -F the tools keep using their original fork()+execv() path; this
   file is then never entered.
*/

#ifndef _HAVE_AFL_TOOL_RUNNER_H
#define _HAVE_AFL_TOOL_RUNNER_H

#include "types.h"

#include <sys/types.h>

/* The tools keep their original fork()+execv() path as the default. The
   forkserver backend lives in a separately linked translation unit. That unit
   provides strong definitions of the functions below; the weak fallback
   stubs at the bottom of this header let the same tool sources link and run
   when the backend is not compiled in. At runtime tool_runner_present()
   tells which one was linked. */

#if defined(__GNUC__) || defined(__clang__)
#  define ATR_WEAK __attribute__((weak))
#else
#  define ATR_WEAK
#endif

/* Result classification for a single candidate execution. The runner keeps
   the raw wait(2) status around as well, so callers can apply their own
   historical crash/exit rules. */

#define ATR_RUN_OK       0          /* Target ran to completion.            */
#define ATR_RUN_TIMEOUT  1          /* Target exceeded the per-run timeout. */
#define ATR_RUN_ERROR    2          /* Backend/protocol failure.            */

struct tool_runner {

  char** argv;                 /* Target argv (already @@-substituted).     */
  u8*    target_path;          /* Binary used for the initial execv().      */
  u8*    trace_bits;           /* Shared coverage bitmap (MAP_SIZE bytes).  */
  u8*    prog_in;              /* Candidate file, always kept on disk so    */
                               /* both stdin and @@/-f targets see it.      */
  s32    dev_null_fd;          /* FD to /dev/null.                          */
  s32    prog_in_fd;           /* Shared descriptor for the candidate file. */
  u64    mem_limit;            /* RLIMIT_AS/DATA in MB (0 = unlimited).     */
  u32    exec_tmout;           /* Per-candidate timeout in ms.              */
  u8     use_stdin;            /* 1: feed candidate on fd 0; 0: /dev/null.  */

  /* Filled in by tool_runner_start(). */

  s32    fsrv_ctl_fd;          /* Forkserver control pipe (write end).      */
  s32    fsrv_st_fd;           /* Forkserver status pipe (read end).        */
  pid_t  forksrv_pid;          /* PID of the forkserver process.            */
  pid_t  child_pid;            /* Current/last target child PID.            */
  u8     persistent;           /* A persistent child is stopped at the loop */
                               /* boundary and gets reused.                 */
  u8     prev_timed_out;       /* Last candidate timed out.                 */
  u8     child_timed_out;      /* Set by the SIGALRM handler.               */
  u8     stopped;              /* Termination requested by a signal.        */
  u8     started;              /* start() succeeded.                        */

};

/* Spin up the forkserver and complete the four-byte handshake. The
   handshake is allowed exec_tmout * FORK_WAIT_MULT milliseconds. Any
   failure is reported through FATAL/PFATAL-style diagnostics and returns a
   nonzero value; the caller must not fall back to direct execution. */

u8 tool_runner_start(struct tool_runner* r);

/* Write the candidate (mem/len) to the on-disk input file and run it once
   through the forkserver. trace_bits are cleared first; on return they hold
   exactly this run's coverage. *status receives the raw wait status (a
   stopped status for a clean persistent iteration). The return value is one
   of ATR_RUN_*. ATR_RUN_ERROR leaves the backend unusable. */

u8 tool_runner_run(struct tool_runner* r, u8* mem, u32 len, int* status);

/* The SIGALRM handler used while waiting for a candidate. Kills the active
   target child (or, when none is outstanding, the forkserver) and records
   the timeout. */

void tool_runner_on_alarm(int sig);

/* Stop signal (SIGHUP/SIGINT/SIGTERM) handler: marks the runner as stopped
   and kills the child/forkserver so blocked pipe operations unwind. */

void tool_runner_on_stop(int sig);

/* Reap the forkserver and any surviving target child, close both pipes and
   release the SHM-backed bitmap. Safe to call multiple times and from an
   atexit handler. */

void tool_runner_destroy(struct tool_runner* r);

/* Returns 1 when the real backend is linked, 0 for the weak stubs. The
   backend translation unit defines the strong version. */

u8 tool_runner_present(void);

/* Weak fallback stubs used only when afl-tool-runner.c is not linked. The
   strong definitions in that translation unit override these. The no-op
   behavior is never reached for real work because -F is rejected while
   tool_runner_present() reports 0. The implementation translation unit
   defines AFL_TOOL_RUNNER_IMPL so these fallback bodies are omitted there. */

#ifndef AFL_TOOL_RUNNER_IMPL

ATR_WEAK u8 tool_runner_start(struct tool_runner* r) { (void)r; return 1; }
ATR_WEAK u8 tool_runner_run(struct tool_runner* r, u8* mem, u32 len,
                            int* status) {
  (void)r; (void)mem; (void)len; (void)status; return ATR_RUN_ERROR;
}
ATR_WEAK void tool_runner_on_alarm(int sig) { (void)sig; }
ATR_WEAK void tool_runner_on_stop(int sig) { (void)sig; }
ATR_WEAK void tool_runner_destroy(struct tool_runner* r) { (void)r; }

/* Returns 1 when the real backend is linked, 0 for the weak stubs. The
   backend defines the strong version of this marker. */

ATR_WEAK u8 tool_runner_present(void) { return 0; }

#endif /* ^AFL_TOOL_RUNNER_IMPL */

#endif /* ! _HAVE_AFL_TOOL_RUNNER_H */
