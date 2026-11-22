/* Observation seams only; absent from normal builds. Apache-2.0. */
#ifndef AFL_TASK_HOOKS_H
#define AFL_TASK_HOOKS_H
static u8 (*afl_task_execute_hook)(char **argv, u8 *data, u32 len);
static void (*afl_task_boundary_hook)(const u8 *map, u32 len);
#endif
