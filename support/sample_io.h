#ifndef SAMPLE_IO_H
#define SAMPLE_IO_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "internal.h"
struct sample_object {
 unsigned char *bytes; size_t size;
 struct dwarf_sections sections;
 uint64_t offsets[DEBUG_MAX];
 struct backtrace_state *state;
 struct dwarf_data *entry;
 fileline lookup;
};
int sample_open(const char *, struct sample_object *);
int sample_read_pcs(const char *, uintptr_t *, size_t, size_t *);
/* No close: libbacktrace borrows section memory for its state lifetime. */
#endif
