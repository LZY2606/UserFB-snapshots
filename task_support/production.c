#include <stdlib.h>
#include "alloc.h"
static size_t allocations;
static void *tracked_malloc(size_t n) { ++allocations; return malloc(n); }
static void *tracked_realloc(void *p, size_t n) { ++allocations; return realloc(p, n); }
void task_alloc_reset(void) { allocations = 0; }
size_t task_alloc_calls(void) { return allocations; }
#define TSF_MALLOC tracked_malloc
#define TSF_REALLOC tracked_realloc
#define TSF_FREE free
#define TSF_IMPLEMENTATION
#include "tsf.h"
