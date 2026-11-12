#ifndef CHECK_CASE_DRIVER_H
#define CHECK_CASE_DRIVER_H
#include <stddef.h>
/* Every registered function returns exactly 1 only after its assertions pass.
 * Zero, 77 and every other value fail; there is no skip result. */
typedef struct { const char *name; int (*run)(void); } test_case;
extern test_case cases[];
extern size_t case_count;
#endif
