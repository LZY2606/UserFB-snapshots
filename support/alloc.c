#include "alloc.h"
#include <stdlib.h>
#undef malloc
#undef calloc
#undef realloc
#undef free
size_t task_allocs, task_frees;
void *task_malloc(size_t n) { ++task_allocs; return malloc(n); }
void *task_calloc(size_t n, size_t bytes) { ++task_allocs; return calloc(n,bytes); }
void *task_realloc(void *p, size_t n) { ++task_allocs; return realloc(p,n); }
void task_free(void *p) { if (p) ++task_frees; free(p); }
