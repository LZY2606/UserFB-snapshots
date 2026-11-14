/* Minimal POSIX configuration for the upstream ELF64 read-only backend.
   Deliberately builds read.c/alloc.c/nounwind.c and no external libraries. */
#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H
#define _GNU_SOURCE 1
#define BACKTRACE_ELF_SIZE 64
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL 1
#define HAVE_LSTAT 1
#define HAVE_READLINK 1
#define HAVE_DECL_STRNLEN 1
#define HAVE_DECL_GETPAGESIZE 1
#define HAVE_SYNC_FUNCTIONS 1
#define HAVE_ATOMIC_FUNCTIONS 1
#endif
