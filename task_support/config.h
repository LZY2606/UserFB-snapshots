/* Portable POSIX core/stdin profile. No preload or platform stdio wrappers. */
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_REGEX_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_FORK 1
#define HAVE_PIPE 1
#define HAVE_WAITPID 1
#define HAVE_SETENV 1
#define HAVE_SETRLIMIT 1
#define HAVE_SIGACTION 1
#define HAVE_KILL 1
#define HAVE_GETOPT_H 1
#define HAVE_GETOPT_LONG 1
#define ATTRIBUTE_PRINTF(x,y) __attribute__((__format__(__printf__,x,y)))
#define PACKAGE_NAME "zzuf"
#define PACKAGE_VERSION "0.15"
#define PACKAGE_STRING "zzuf 0.15"
#define VERSION "0.15"
#define SONAME "libzzuf.so"
