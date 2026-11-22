/* Baseline input consumer; uses the upstream AFL SHM runtime directly.
 * No forkserver client or future -F implementation is supplied here. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
extern unsigned char *__afl_area_ptr;
int main(int argc, char **argv) {
  unsigned char buf[256];
  size_t used = 0;
  int fd = 0;
  const char *log = getenv("AFL_BASELINE_START_LOG");
  if (log) {
    int out = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (out < 0) return 70;
    char line[64];
    int n = snprintf(line, sizeof(line), "%ld\n", (long)getpid());
    if (write(out, line, (size_t)n) != n) return 71;
    close(out);
  }
  if (argc > 1 && (fd = open(argv[1], O_RDONLY)) < 0) return 72;
  while (used < sizeof(buf)) {
    ssize_t n = read(fd, buf + used, sizeof(buf) - used);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return 73;
    if (!n) break;
    used += (size_t)n;
  }
  __afl_area_ptr[17]++;
  __afl_area_ptr[used && buf[0] == 'A' ? 31 : 47]++;
  if (fd) close(fd);
  return 0;
}
