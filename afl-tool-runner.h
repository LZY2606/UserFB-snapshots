/*
  afl-tool-runner - shared target execution backend for afl-tmin and
  afl-analyze.

  The default backend preserves the classic fork()+execve() behavior. When
  AFL_TOOL_RUNNER_FORKSRV is requested, one forkserver is started for the
  lifetime of the tool and every candidate is requested through the usual
  AFL control/status pipe protocol.
*/

#ifndef _AFL_TOOL_RUNNER_H
#define _AFL_TOOL_RUNNER_H

#include "types.h"

#include <sys/types.h>

enum afl_tool_runner_mode {

  AFL_TOOL_RUNNER_EXEC = 0,
  AFL_TOOL_RUNNER_FORKSRV = 1

};

struct afl_tool_runner_options {

  enum afl_tool_runner_mode mode;

  const u8 *target_path;
  char * const *argv;

  /* Candidate file used with -f / @@ and, in stdin mode, as the persistent
     regular file cloned onto fd 0. */
  const u8 *prog_in;

  u8 *trace_bits;
  s32 dev_null_fd;

  u32 timeout_ms;
  u64 mem_limit_mb;

  u8 use_stdin;
  u8 new_session;

  /* These normally point at the per-tool signal handling state. The runner
     updates *child_pid while a blocking operation is in progress so that
     the existing SIGALRM/SIGTERM handlers can kill the right process. */
  volatile s32 *child_pid;
  volatile u8 *timed_out;
  volatile u8 *stop_soon;

};

typedef struct afl_tool_runner afl_tool_runner;

afl_tool_runner *afl_tool_runner_create(
    const struct afl_tool_runner_options *opts);

/* In forkserver mode, perform the initial handshake. Returns zero on
   success and -1 on a fatal protocol/execution error. If interruption was
   requested, *opts.stop_soon is non-zero. */
int afl_tool_runner_start(afl_tool_runner *r);

/* Write candidate bytes and execute one round. The raw wait status is
   returned through *status. Returns zero on success and -1 if the
   forkserver protocol or target execution setup is unrecoverable. */
int afl_tool_runner_run(afl_tool_runner *r, const u8 *mem, u32 len,
                        int *status);

const char *afl_tool_runner_error(const afl_tool_runner *r);

/* Kill/reap owned processes and close channels. Safe to call more than
   once and from an atexit handler. */
void afl_tool_runner_destroy(afl_tool_runner **rp);

#endif /* !_AFL_TOOL_RUNNER_H */
