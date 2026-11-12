#ifndef SESSION_DRIVER_H
#define SESSION_DRIVER_H
#include <stddef.h>
#include <stdint.h>
enum session_group { ASSOCIATION, HEAD_REUSE, HANDOFF, SESSION_GROUPS };
struct session_case { const char *name; enum session_group group; int (*run)(void); };
/* run(): 1=executed and passed, 0=skipped, negative=failed. */
const struct session_case *session_cases(size_t *count);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
#endif
