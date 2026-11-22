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

   Drives the forkserver embedded in the LLVM-instrumented runtime, so that
   afl-tmin and afl-analyze can reuse a single target initialization. See
   afl-tool-runner.h for the protocol overview.
*/

#include "android-ashmem.h"

#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>

#define AFL_TOOL_RUNNER_IMPL
#include "afl-tool-runner.h"

/* The signal handlers need to reach the live runner without the tools having
   to pass context through a signal handler. */

static struct tool_runner* volatile g_runner;

/* Strong marker overriding the weak stub in afl-tool-runner.h. */

u8 tool_runner_present(void) { return 1; }

/* Exact four-byte pipe transfers. Returns 4 on success, 0 on EOF, -1 on an
   interrupted/failed transfer. */

static s32 write_full(s32 fd, const void* buf) {

  const u8* p = (const u8*)buf;
  u32 done = 0;

  while (done < 4) {

    s32 n = write(fd, p + done, 4 - done);

    if (n < 0) {

      if (errno == EINTR) continue;
      return -1;

    }

    if (!n) return 0;
    done += (u32)n;

  }

  return 4;

}


static s32 read_full(s32 fd, void* buf) {

  u8* p = (u8*)buf;
  u32 done = 0;

  while (done < 4) {

    s32 n = read(fd, p + done, 4 - done);

    if (n < 0) {

      if (errno == EINTR) continue;
      return -1;

    }

    if (!n) return (s32)done;   /* 0: clean EOF; 1..3: truncated frame */
    done += (u32)n;

  }

  return 4;

}


/* Portable byte-pattern search (memmem() is not guaranteed on Darwin). */

static u8* find_sig(u8* haystack, size_t hlen, const char* needle) {

  size_t nlen = strlen(needle);

  if (!nlen || hlen < nlen) return NULL;

  for (size_t i = 0; i + nlen <= hlen; i++) {

    if (haystack[i] == (u8)needle[0] &&
        !memcmp(haystack + i, needle, nlen)) return haystack + i;

  }

  return NULL;

}


/* Detect the persistent/deferred signatures baked into the binary and export
   the runtime environment variables, mirroring afl-fuzz's check_binary(). */

static void detect_runtime_modes(struct tool_runner* r) {

  s32 fd = open(r->target_path, O_RDONLY);

  if (fd < 0) PFATAL("Unable to open '%s'", r->target_path);

  struct stat st;

  if (fstat(fd, &st) || !st.st_size) FATAL("Unable to stat '%s'", r->target_path);

  u8* img = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (img == MAP_FAILED) PFATAL("mmap() failed for '%s'", r->target_path);

  close(fd);

  size_t img_len = (size_t)st.st_size;

  if (find_sig(img, img_len, PERSIST_SIG)) {

    OKF(cPIN "Persistent mode binary detected.");
    setenv(PERSIST_ENV_VAR, "1", 1);

  }

  if (find_sig(img, img_len, DEFER_SIG)) {

    OKF(cPIN "Deferred forkserver binary detected.");
    setenv(DEFER_ENV_VAR, "1", 1);

  }

  if (munmap(img, img_len)) PFATAL("munmap() failed");

}


void tool_runner_on_alarm(int sig) {

  struct tool_runner* r = g_runner;

  (void)sig;

  if (!r) return;

  r->child_timed_out = 1;

  if (r->child_pid > 0)
    kill(r->child_pid, SIGKILL);
  else if (r->forksrv_pid > 0)
    kill(r->forksrv_pid, SIGKILL);

}


void tool_runner_on_stop(int sig) {

  struct tool_runner* r = g_runner;

  (void)sig;

  if (!r) return;

  r->stopped = 1;

  if (r->child_pid > 0) kill(r->child_pid, SIGKILL);
  if (r->forksrv_pid > 0) kill(r->forksrv_pid, SIGKILL);

}


/* Arm/disarm the per-candidate wall-clock timer. */

static void arm_timer(u32 ms) {

  struct itimerval it;

  it.it_value.tv_sec  = (ms / 1000);
  it.it_value.tv_usec = (ms % 1000) * 1000;
  it.it_interval.tv_sec = 0;
  it.it_interval.tv_usec = 0;

  setitimer(ITIMER_REAL, &it, NULL);

}


static void disarm_timer(void) {

  struct itimerval it;

  memset(&it, 0, sizeof(it));
  setitimer(ITIMER_REAL, &it, NULL);

}


u8 tool_runner_start(struct tool_runner* r) {

  int st_pipe[2], ctl_pipe[2];
  s32 rlen;
  s32 hello;

  r->fsrv_ctl_fd = r->fsrv_st_fd = -1;
  r->prog_in_fd = -1;
  r->forksrv_pid = r->child_pid = 0;
  r->persistent = r->prev_timed_out = r->child_timed_out = 0;
  r->stopped = r->started = 0;

  detect_runtime_modes(r);

  /* One persistent descriptor for the candidate file. It is rewritten in
     place (rewind + truncate) before every run, so stdin children and
     path-based children always see the current bytes and a real EOF. */

  r->prog_in_fd = open(r->prog_in, O_RDWR | O_CREAT, 0600);

  if (r->prog_in_fd < 0) PFATAL("Unable to create '%s'", r->prog_in);

  if (pipe(st_pipe) || pipe(ctl_pipe)) PFATAL("pipe() failed");

  r->forksrv_pid = fork();

  if (r->forksrv_pid < 0) PFATAL("fork() failed");

  if (!r->forksrv_pid) {

    struct rlimit rlim;

    if (!getrlimit(RLIMIT_NOFILE, &rlim) && rlim.rlim_cur < FORKSRV_FD + 2) {

      rlim.rlim_cur = FORKSRV_FD + 2;
      setrlimit(RLIMIT_NOFILE, &rlim); /* Ignore errors */

    }

    if (r->mem_limit) {

      rlim.rlim_max = rlim.rlim_cur = ((rlim_t)r->mem_limit) << 20;

#ifdef RLIMIT_AS
      setrlimit(RLIMIT_AS, &rlim); /* Ignore errors */
#else
      setrlimit(RLIMIT_DATA, &rlim); /* Ignore errors */
#endif /* ^RLIMIT_AS */

    }

    rlim.rlim_max = rlim.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &rlim); /* Ignore errors */

    setsid();

    if (dup2(r->dev_null_fd, 1) < 0 ||
        dup2(r->dev_null_fd, 2) < 0) {

      *(u32*)r->trace_bits = EXEC_FAIL_SIG;
      PFATAL("dup2() failed");

    }

    if (r->use_stdin) {

      if (dup2(r->prog_in_fd, 0) < 0) {

        *(u32*)r->trace_bits = EXEC_FAIL_SIG;
        PFATAL("dup2() failed");

      }

    } else if (dup2(r->dev_null_fd, 0) < 0) {

      *(u32*)r->trace_bits = EXEC_FAIL_SIG;
      PFATAL("dup2() failed");

    }

    if (dup2(ctl_pipe[0], FORKSRV_FD) < 0 ||
        dup2(st_pipe[1], FORKSRV_FD + 1) < 0) {

      *(u32*)r->trace_bits = EXEC_FAIL_SIG;
      PFATAL("dup2() failed");

    }

    close(ctl_pipe[0]);
    close(ctl_pipe[1]);
    close(st_pipe[0]);
    close(st_pipe[1]);
    close(r->prog_in_fd);
    close(r->dev_null_fd);

    if (!getenv("LD_BIND_LAZY")) setenv("LD_BIND_NOW", "1", 0);

    execv(r->target_path, r->argv);

    /* execv() returned: the target could not be started. */

    *(u32*)r->trace_bits = EXEC_FAIL_SIG;
    exit(0);

  }

  close(ctl_pipe[0]);
  close(st_pipe[1]);

  r->fsrv_ctl_fd = ctl_pipe[1];
  r->fsrv_st_fd  = st_pipe[0];

  g_runner = r;

  /* The handshake gets ten times the per-run timeout, matching afl-fuzz. */

  arm_timer(r->exec_tmout * FORK_WAIT_MULT);

  rlen = read_full(r->fsrv_st_fd, &hello);

  disarm_timer();

  if (rlen == 4) {

    r->started = 1;
    OKF("All right - fork server is up.");
    return 0;

  }

  /* Something went wrong before the hello frame arrived. */

  r->started = 0;
  g_runner = NULL;

  close(r->fsrv_ctl_fd);
  close(r->fsrv_st_fd);
  r->fsrv_ctl_fd = r->fsrv_st_fd = -1;

  if (r->child_timed_out)
    FATAL("Timeout while initializing fork server (adjusting -t may help)");

  int wstatus = 0;

  if (waitpid(r->forksrv_pid, &wstatus, 0) <= 0)
    PFATAL("waitpid() failed");

  if (*(u32*)r->trace_bits == EXEC_FAIL_SIG)
    FATAL("Unable to execute '%s'", r->argv[0]);

  if (rlen > 0 && rlen != 4)
    FATAL("Fork server handshake failed: incomplete frame (%d/4 bytes)", rlen);

  FATAL("Fork server handshake failed");

}


/* Replace the on-disk candidate through the shared descriptor. Truncation
   guarantees that a shorter candidate cannot leave trailing bytes behind and
   that every reader observes a proper EOF. */

static void rewrite_candidate(struct tool_runner* r, u8* mem, u32 len) {

  if (lseek(r->prog_in_fd, 0, SEEK_SET) < 0) PFATAL("lseek() failed");
  if (ftruncate(r->prog_in_fd, (off_t)len)) PFATAL("ftruncate() failed");

  if (len) ck_write(r->prog_in_fd, mem, len, r->prog_in);

  if (lseek(r->prog_in_fd, 0, SEEK_SET) < 0) PFATAL("lseek() failed");

}


u8 tool_runner_run(struct tool_runner* r, u8* mem, u32 len, int* status) {

  u32 was_killed;
  s32 pid;
  s32 res;

  if (!r->started) return ATR_RUN_ERROR;

  r->child_timed_out = 0;

  /* Every iteration starts from a clean bitmap. */

  memset(r->trace_bits, 0, MAP_SIZE);
  MEM_BARRIER();

  rewrite_candidate(r, mem, len);

  arm_timer(r->exec_tmout);

  was_killed = r->prev_timed_out;

  res = write_full(r->fsrv_ctl_fd, &was_killed);

  if (res != 4) {

    disarm_timer();

    if (r->stopped) return ATR_RUN_ERROR;
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  res = read_full(r->fsrv_st_fd, &pid);

  if (res != 4) {

    disarm_timer();

    if (r->stopped) return ATR_RUN_ERROR;
    if (r->child_timed_out)
      FATAL("Timeout while waiting for fork server PID (adjusting -t may help)");
    if (res >= 0 && res != 4)
      FATAL("Fork server sent an incomplete PID frame (%d/4 bytes)", res);
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  if (pid <= 0) FATAL("Fork server is misbehaving (invalid PID %d)", pid);

  r->child_pid = (pid_t)pid;

  /* If the candidate overruns the timeout, the SIGALRM handler SIGKILLs the
     child. The runtime is blocked in waitpid() for that very child, so the
     kill unblocks it and it still relays the resulting status frame. Reading
     that frame here keeps both pipes synchronized and lets the next
     candidate run normally. */

  res = read_full(r->fsrv_st_fd, status);

  disarm_timer();

  if (res != 4) {

    r->child_pid = 0;

    if (r->stopped) return ATR_RUN_ERROR;
    if (r->child_timed_out)
      FATAL("Timeout while talking to fork server (adjusting -t may help)");
    if (res >= 0 && res != 4)
      FATAL("Fork server sent an incomplete status frame (%d/4 bytes)", res);
    RPFATAL(res, "Unable to communicate with fork server (OOM?)");

  }

  /* A stopped child is parked at a persistent-loop boundary: a normal
     completion, and the same process gets resumed on the next request. */

  if (WIFSTOPPED(*status)) {

    r->persistent = 1;
    /* Keep child_pid so the alarm/stop handlers can reach it. */

  } else {

    r->persistent = 0;
    r->child_pid = 0;

  }

  r->prev_timed_out = r->child_timed_out;

  MEM_BARRIER();

  if (r->child_timed_out) return ATR_RUN_TIMEOUT;

  return ATR_RUN_OK;

}


void tool_runner_destroy(struct tool_runner* r) {

  if (!r) return;

  g_runner = NULL;

  if (r->child_pid > 0) {

    kill(r->child_pid, SIGKILL);
    waitpid(r->child_pid, NULL, 0);
    r->child_pid = 0;

  }

  if (r->forksrv_pid > 0) {

    kill(r->forksrv_pid, SIGKILL);
    waitpid(r->forksrv_pid, NULL, 0);
    r->forksrv_pid = 0;

  }

  if (r->fsrv_ctl_fd >= 0) {

    close(r->fsrv_ctl_fd);
    r->fsrv_ctl_fd = -1;

  }

  if (r->fsrv_st_fd >= 0) {

    close(r->fsrv_st_fd);
    r->fsrv_st_fd = -1;

  }

  if (r->prog_in_fd >= 0) {

    close(r->prog_in_fd);
    r->prog_in_fd = -1;

  }

  if (r->trace_bits) {

    shmdt(r->trace_bits);
    r->trace_bits = NULL;

  }

  r->started = 0;

}
