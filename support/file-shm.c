/* Local portability shim for hosts that deny SysV SHM. It implements only the
 * IPC_PRIVATE + shmat + IPC_RMID operations used by these AFL tools/runtime.
 * Separate processes map the same file MAP_SHARED; the wire bitmap is unchanged.
 * Build with shmget/shmat/shmctl redirected to check_* names. */
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
static int shm_path(char *out, size_t n, int id) {
  const char *root = getenv("SHM_ROOT");
  if (!root || id <= 0) { errno = EINVAL; return -1; }
  int written = snprintf(out, n, "%s/map-%d", root, id);
  if (written < 0 || (size_t)written >= n) { errno = ENAMETOOLONG; return -1; }
  return 0;
}
int check_shmget(key_t key, size_t size, int flags) {
  char path[PATH_MAX];
  int id = (int)getpid();
  if (key != IPC_PRIVATE || !size || !(flags & IPC_CREAT)) { errno = EINVAL; return -1; }
  if (shm_path(path, sizeof(path), id)) return -1;
  int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return -1;
  int rc = ftruncate(fd, (off_t)size), saved = errno;
  close(fd);
  if (rc) { unlink(path); errno = saved; return -1; }
  return id;
}
void *check_shmat(int id, const void *addr, int flags) {
  char path[PATH_MAX];
  struct stat st;
  if (addr || flags || shm_path(path, sizeof(path), id)) { errno = EINVAL; return (void *)-1; }
  int fd = open(path, O_RDWR);
  if (fd < 0) return (void *)-1;
  if (fstat(fd, &st) || st.st_size <= 0) { close(fd); errno = EINVAL; return (void *)-1; }
  void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  int saved = errno;
  close(fd);
  errno = saved;
  return p;
}
int check_shmctl(int id, int command, struct shmid_ds *buf) {
  char path[PATH_MAX];
  if (command != IPC_RMID || buf) { errno = EINVAL; return -1; }
  if (shm_path(path, sizeof(path), id)) return -1;
  return unlink(path);
}
