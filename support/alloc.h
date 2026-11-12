#ifndef TASK_ALLOC_H
#define TASK_ALLOC_H
#include <stddef.h>
#include <stdlib.h>
void *task_malloc(size_t n);
void *task_calloc(size_t n, size_t bytes);
void *task_realloc(void *p, size_t n);
void task_free(void *p);
extern size_t task_allocs;
extern size_t task_frees;
#define KISS_FFT_MALLOC task_malloc
#define KISS_FFT_FREE task_free
#define malloc task_malloc
#define calloc task_calloc
#define realloc task_realloc
#define free task_free
#endif
