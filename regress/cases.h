#ifndef CHECK_CASES_H
#define CHECK_CASES_H
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*task_fn)(void);
void task_case(const char*, task_fn);
void task_register_tests(void);
#ifdef __cplusplus
}
#endif
#endif
