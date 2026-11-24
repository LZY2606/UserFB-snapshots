/*
  afl-tool-runner.c - shared execution backend for afl-tmin/afl-analyze.
*/

#include "afl-tool-runner.h"

#include "alloc-inl.h"
#include "config.h"
#include "debug.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

struct afl_tool_runner {

  struct afl_tool_runner_options o;

  char err[256];

  pid_t server_pid;
  pid_t child_pid;

  s32 ctl_fd;
  s32 st_fd;
  s32 input_fd;

  u8 started;
  u8 last_timed_out;
  u8 child_is_stopped;

};

static afl_tool_runner *active_runner;


static void runner_atexit(void) {

  if (active_runner) afl_tool_runner_destroy(&active_runner);

}


static void set_error(afl_tool_runner *r, const char *fmt, ...) {

  va_list ap;

  va_start(ap, fmt);
  vsnprintf((char *)r->err, sizeof(r->err), fmt, ap);
  va_end(ap);

}


const char *afl_tool_runner_error(const afl_tool_runner *r) {

  return r->err[0] ? (const char *)r->err : "unknown runner error";

}


afl_tool_runner *afl_tool_runner_create(
    const struct afl_tool_runner_options *opts) {

  afl_tool_runner *r;

  if (!opts || !opts->argv || !opts->target_path || !opts->prog_in) {
    FATAL("Invalid afl-tool-runner configuration");
  }

  if (active_runner) afl_tool_runner_destroy(&active_runner);

  r = ck_alloc(sizeof(afl_tool_runner));
  r->o = *opts;
  r->server_pid = -1;
  r->child_pid = -1;
  r->ctl_fd = -1;
  r->st_fd = -1;
  r->input_fd = -1;

  active_runner = r;
  atexit(runner_atexit);

  return r;

}


static void arm_timer(u64 timeout_ms) {

  struct itimerval it;

  memset(&it, 0, sizeof(it));
  it.it_value.tv_sec = timeout_ms / 1000;
  it.it_value.tv_usec = (timeout_ms % 1000) * 1000;

  if (setitimer(ITIMER_REAL, &it, NULL) < 0) PFATAL("setitimer() failed");

}


static void disarm_timer(void) {

  struct itimerval it;

  memset(&it, 0, sizeof(it));
  setitimer(ITIMER_REAL, &it, NULL);

}


static ssize_t write_all(s32 fd, const void *buf, size_t len) {

  const u8 *p = buf;
  size_t done = 0;

  while (done < len) {

    ssize_t n = write(fd, p + done, len - done);

    if (n > 0) {

      done += (size_t)n;

    } else if (!n) {

      errno = EIO;
      return -1;

    } else if (errno != EINTR) return -1;

  }

  return (ssize_t)done;

}


/* Read a fixed-size protocol frame. Returns the number of bytes read before
   EOF (zero to four), or -1 on an I/O error. A short positive return is a
   malformed/incomplete frame. */
static ssize_t read_frame(s32 fd, u32 *frame) {

  u8 *out = (u8 *)frame;
  size_t done = 0;

  while (done < 4) {

    ssize_t n = read(fd, out + done, 4 - done);

    if (n > 0) {

      done += (size_t)n;

    } else if (!n) {

      break;

    } else if (errno == EINTR) {

      continue;

    } else {

      return -1;

    }

  }

  return (ssize_t)done;

}


static void apply_resource_limits(const afl_tool_runner *r) {

  struct rlimit lim;

  if (r->o.mem_limit_mb) {

    lim.rlim_max = lim.rlim_cur = ((rlim_t)r->o.mem_limit_mb) << 20;

#ifdef RLIMIT_AS
    setrlimit(RLIMIT_AS, &lim); /* Ignore errors. */
#else
    setrlimit(RLIMIT_DATA, &lim); /* Ignore errors. */
#endif /* ^RLIMIT_AS */

  }

  lim.rlim_max = lim.rlim_cur = 0;
  setrlimit(RLIMIT_CORE, &lim); /* Ignore errors. */

}


static s32 create_candidate_file(const afl_tool_runner *r, const u8 *mem,
                                u32 len) {

  s32 fd;

  unlink((char *)r->o.prog_in); /* Ignore errors. */

  fd = open((char *)r->o.prog_in, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {

    set_error((afl_tool_runner *)r, "Unable to create '%s': %s",
              r->o.prog_in, strerror(errno));
    return -1;

  }

  if (len && write_all(fd, mem, len) != len) {

    set_error((afl_tool_runner *)r, "Short write to '%s'", r->o.prog_in);
    close(fd);
    return -1;

  }

  if (lseek(fd, 0, SEEK_SET) < 0) {

    set_error((afl_tool_runner *)r, "Unable to rewind '%s': %s",
              r->o.prog_in, strerror(errno));
    close(fd);
    return -1;

  }

  return fd;

}


/* Rewind, overwrite, and truncate the same open file description used by the
   forkserver descendants as fd 0. Truncation is what makes shorter/empty
   candidates observe EOF instead of the previous input's tail. */
static s32 prepare_persistent_stdin(afl_tool_runner *r, const u8 *mem,
                                    u32 len) {

  if (r->input_fd < 0) {

    unlink((char *)r->o.prog_in); /* Ignore errors. */
    r->input_fd = open((char *)r->o.prog_in, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (r->input_fd < 0) {

      set_error(r, "Unable to create '%s': %s", r->o.prog_in,
                strerror(errno));
      return -1;

    }

  }

  if (lseek(r->input_fd, 0, SEEK_SET) < 0 || ftruncate(r->input_fd, 0) < 0) {

    set_error(r, "Unable to rewind '%s': %s", r->o.prog_in, strerror(errno));
    return -1;

  }

  if (len && write_all(r->input_fd, mem, len) != len) {

    set_error(r, "Short write to '%s'", r->o.prog_in);
    return -1;

  }

  if (ftruncate(r->input_fd, (off_t)len) < 0 ||
      lseek(r->input_fd, 0, SEEK_SET) < 0) {

    set_error(r, "Unable to resize '%s': %s", r->o.prog_in, strerror(errno));
    return -1;

  }

  return 0;

}


static int run_direct(afl_tool_runner *r, const u8 *mem, u32 len,
                      int *status) {

  pid_t pid;
  s32 candidate_fd;

  candidate_fd = create_candidate_file(r, mem, len);
  if (candidate_fd < 0) return -1;

  pid = fork();

  if (pid < 0) {

    close(candidate_fd);
    set_error(r, "fork() failed: %s", strerror(errno));
    return -1;

  }

  if (!pid) {

    if (dup2(r->o.use_stdin ? candidate_fd : r->o.dev_null_fd, 0) < 0 ||
        dup2(r->o.dev_null_fd, 1) < 0 ||
        dup2(r->o.dev_null_fd, 2) < 0) {

      *(u32 *)r->o.trace_bits = EXEC_FAIL_SIG;
      _exit(120);

    }

    if (r->o.dev_null_fd > 2) close(r->o.dev_null_fd);
    if (candidate_fd > 2) close(candidate_fd);

    if (r->o.new_session) setsid();

    apply_resource_limits(r);

    execv((const char *)r->o.target_path, r->o.argv);

    *(u32 *)r->o.trace_bits = EXEC_FAIL_SIG;
    _exit(0);

  }

  close(candidate_fd);

  if (r->o.child_pid) *r->o.child_pid = (s32)pid;
  if (r->o.timed_out) *r->o.timed_out = 0;

  arm_timer(r->o.timeout_ms);

  while (waitpid(pid, status, 0) < 0) {

    if (errno == EINTR) continue;

    disarm_timer();
    if (r->o.child_pid) *r->o.child_pid = 0;
    set_error(r, "waitpid() failed: %s", strerror(errno));
    return -1;

  }

  disarm_timer();
  if (r->o.child_pid) *r->o.child_pid = 0;

  return 0;

}


/* Move a pipe endpoint above the fixed forkserver descriptor range so that
   dup2() onto 198/199 cannot destroy another endpoint. */
static s32 raise_fd(s32 fd) {

  s32 nfd;

  if (fd >= FORKSRV_FD + 10) return fd;

  nfd = fcntl(fd, F_DUPFD, FORKSRV_FD + 10);
  if (nfd < 0) return -1;

  close(fd);
  return nfd;

}


static void forkserver_child(afl_tool_runner *r, s32 ctl_read,
                             s32 ctl_write, s32 st_read, s32 st_write) {

  struct rlimit nofile;

  if (!getrlimit(RLIMIT_NOFILE, &nofile) &&
      nofile.rlim_cur < FORKSRV_FD + 2) {

    nofile.rlim_cur = FORKSRV_FD + 2;
    setrlimit(RLIMIT_NOFILE, &nofile); /* Ignore errors. */

  }

  if (r->o.new_session) setsid();

  dup2(r->o.dev_null_fd, 1);
  dup2(r->o.dev_null_fd, 2);

  if (r->o.use_stdin) dup2(r->input_fd, 0);
  else dup2(r->o.dev_null_fd, 0);

  if (r->o.dev_null_fd > 2) close(r->o.dev_null_fd);
  if (r->input_fd > 2) close(r->input_fd);

  close(ctl_write);
  close(st_read);

  if (dup2(ctl_read, FORKSRV_FD) < 0) _exit(121);
  if (dup2(st_write, FORKSRV_FD + 1) < 0) _exit(122);

  if (ctl_read != FORKSRV_FD) close(ctl_read);
  if (st_write != FORKSRV_FD + 1) close(st_write);

  apply_resource_limits(r);

  execv((const char *)r->o.target_path, r->o.argv);

  /* The parent distinguishes this from an ordinary target exit via the
     bitmap signature and the failed handshake/EOF. */
  *(u32 *)r->o.trace_bits = EXEC_FAIL_SIG;
  _exit(0);

}


int afl_tool_runner_start(afl_tool_runner *r) {

  s32 ctl_pipe[2], st_pipe[2];
  u32 hello;
  ssize_t n;

  if (!r) return -1;
  if (r->o.mode != AFL_TOOL_RUNNER_FORKSRV) return 0;
  if (r->started) return 0;

  /* The upstream runtime enables __AFL_LOOP() only when this is present.
     Setting it for a target which never calls the loop is harmless. */
  setenv(PERSIST_ENV_VAR, "1", 1);

  if (r->o.use_stdin && prepare_persistent_stdin(r, (const u8 *)"", 0) < 0)
    return -1;

  if (pipe(ctl_pipe) || pipe(st_pipe)) {

    set_error(r, "pipe() failed: %s", strerror(errno));
    return -1;

  }

  ctl_pipe[0] = raise_fd(ctl_pipe[0]);
  ctl_pipe[1] = raise_fd(ctl_pipe[1]);
  st_pipe[0] = raise_fd(st_pipe[0]);
  st_pipe[1] = raise_fd(st_pipe[1]);

  if (ctl_pipe[0] < 0 || ctl_pipe[1] < 0 ||
      st_pipe[0] < 0 || st_pipe[1] < 0) {

    set_error(r, "fcntl(F_DUPFD) failed: %s", strerror(errno));
    return -1;

  }

  r->server_pid = fork();

  if (r->server_pid < 0) {

    set_error(r, "fork() failed: %s", strerror(errno));
    return -1;

  }

  if (!r->server_pid) {

    forkserver_child(r, ctl_pipe[0], ctl_pipe[1], st_pipe[0], st_pipe[1]);
    _exit(123);

  }

  close(ctl_pipe[0]);
  close(st_pipe[1]);

  r->ctl_fd = ctl_pipe[1];
  r->st_fd = st_pipe[0];

  if (r->o.child_pid) *r->o.child_pid = (s32)r->server_pid;
  if (r->o.timed_out) *r->o.timed_out = 0;

  arm_timer((u64)r->o.timeout_ms * FORK_WAIT_MULT);
  n = read_frame(r->st_fd, &hello);
  disarm_timer();

  if (r->o.child_pid) *r->o.child_pid = 0;

  if (r->o.stop_soon && *r->o.stop_soon) {

    set_error(r, "forkserver startup interrupted");
    return -1;

  }

  if (r->o.timed_out && *r->o.timed_out) {

    set_error(r, "timeout while initializing fork server "
                 "(increase -t; handshake limit is %u ms)",
              (u32)((u64)r->o.timeout_ms * FORK_WAIT_MULT));
    return -1;

  }

  if (n != 4) {

    if (*(u32 *)r->o.trace_bits == EXEC_FAIL_SIG)
      set_error(r, "Unable to execute target application '%s'",
                r->o.argv[0]);
    else if (n < 0)
      set_error(r, "forkserver handshake read failed: %s", strerror(errno));
    else if (n == 0)
      set_error(r, "forkserver closed the status pipe before handshake");
    else
      set_error(r, "incomplete forkserver handshake frame (%zd/4 bytes)", n);

    return -1;

  }

  r->started = 1;
  return 0;

}


static int run_forkserver(afl_tool_runner *r, const u8 *mem, u32 len,
                          int *status) {

  u32 was_killed = r->last_timed_out ? 1 : 0;
  u32 pid_word, status_word;
  ssize_t n;
  pid_t pid;

  if (!r->started) {

    set_error(r, "forkserver is not running");
    return -1;

  }

  if (r->o.use_stdin) {

    if (prepare_persistent_stdin(r, mem, len) < 0) return -1;

  } else {

    s32 fd = create_candidate_file(r, mem, len);

    if (fd < 0) return -1;
    close(fd);

  }

  if (write_all(r->ctl_fd, &was_killed, 4) != 4) {

    set_error(r, "Unable to request a new process from fork server: %s",
              strerror(errno));
    return -1;

  }

  n = read_frame(r->st_fd, &pid_word);

  if (n != 4) {

    if (n < 0)
      set_error(r, "Unable to read PID from fork server: %s",
                strerror(errno));
    else
      set_error(r, "incomplete forkserver PID frame (%zd/4 bytes)", n);

    return -1;

  }

  pid = (pid_t)(s32)pid_word;

  if (pid <= 0) {

    set_error(r, "fork server returned invalid child PID %d", (int)pid);
    return -1;

  }

  r->child_pid = pid;
  r->child_is_stopped = 0;
  if (r->o.child_pid) *r->o.child_pid = (s32)pid;
  if (r->o.timed_out) *r->o.timed_out = 0;

  arm_timer(r->o.timeout_ms);
  n = read_frame(r->st_fd, &status_word);
  disarm_timer();

  if (n != 4) {

    if (r->o.child_pid) *r->o.child_pid = 0;

    if (n < 0)
      set_error(r, "Unable to read status from fork server: %s",
                strerror(errno));
    else
      set_error(r, "incomplete forkserver status frame (%zd/4 bytes)", n);

    return -1;

  }

  *status = (int)status_word;
  r->last_timed_out = (r->o.timed_out && *r->o.timed_out) ? 1 : 0;

  if (WIFSTOPPED(*status)) {

    /* A persistent target is parked at __AFL_LOOP(); keep its PID available
       for interruption/cleanup. This status is a normal round boundary. */
    r->child_is_stopped = 1;

  } else {

    r->child_pid = -1;
    if (r->o.child_pid) *r->o.child_pid = 0;

  }

  return 0;

}


int afl_tool_runner_run(afl_tool_runner *r, const u8 *mem, u32 len,
                        int *status) {

  if (!r || !status) return -1;

  r->err[0] = 0;
  *status = 0;

  if (r->o.mode == AFL_TOOL_RUNNER_FORKSRV)
    return run_forkserver(r, mem, len, status);

  return run_direct(r, mem, len, status);

}


void afl_tool_runner_destroy(afl_tool_runner **rp) {

  afl_tool_runner *r;

  if (!rp || !*rp) return;

  r = *rp;
  *rp = NULL;

  if (active_runner == r) active_runner = NULL;

  /* Kill a parked persistent or still-running target. It is a child of the
     fork server, so the server is responsible for the formal reap below. */
  if (r->child_pid > 0) kill(r->child_pid, SIGKILL);

  if (r->ctl_fd >= 0) {

    /* If a persistent child was parked, tell the runtime that the SIGKILL
       raced with its stop so that it reaps it instead of SIGCONTing it. */
    if (r->child_is_stopped) {
      u32 was_killed = 1;
      (void)write(r->ctl_fd, &was_killed, sizeof(was_killed));
    }

    close(r->ctl_fd);

  }

  if (r->st_fd >= 0) close(r->st_fd);
  if (r->input_fd >= 0) close(r->input_fd);

  if (r->server_pid > 0) {

    pid_t got;

    /* Closing ctl normally makes the runtime exit on its next pipe read.
       Do not block on a malformed/test server: force termination. */
    got = waitpid(r->server_pid, NULL, WNOHANG);
    if (got == 0) kill(r->server_pid, SIGKILL);

    while (waitpid(r->server_pid, NULL, 0) < 0 && errno == EINTR) {
    }

  }

  ck_free(r);

}
