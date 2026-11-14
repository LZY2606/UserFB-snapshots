#ifndef TASK_SERIAL_OMP_H
#define TASK_SERIAL_OMP_H
/* Build-time single-worker portability adapter, not an OpenMP runtime. */
static inline int omp_get_max_threads(void) { return 1; }
static inline int omp_get_thread_num(void) { return 0; }
#endif
