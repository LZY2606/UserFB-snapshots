/* C test registration contract. Added locally, zlib license. */
#ifndef CHECK_CASE_H
#define CHECK_CASE_H
#include <stddef.h>
typedef struct PagedCase { const char *name; int (*run)(void); } PagedCase;
extern const PagedCase paged_cases[];
extern const size_t paged_case_count;
#endif
