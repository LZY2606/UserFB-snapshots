#ifndef CHECK_CASES_H
#define CHECK_CASES_H
#include <stddef.h>
typedef struct { const char *group; const char *name; void (*run)(void); } TargetCase;
extern const TargetCase target_cases[];
extern const size_t target_count;
void target_check(int passed, const char *expr, int line);
#define TARGET_CHECK(expr) target_check(!!(expr), #expr, __LINE__)
#endif
