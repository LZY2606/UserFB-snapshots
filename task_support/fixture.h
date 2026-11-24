#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <stddef.h>
/* Builds one valid synthetic ELF64 little-endian ET_REL. Caller owns bytes. */
unsigned char *task_fixture(int compressed, size_t *length);
#endif
