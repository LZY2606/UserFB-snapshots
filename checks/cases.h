#ifndef CHECK_CASES_H
#define CHECK_CASES_H
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*task_fn)(void);
void task_case(const char *name, task_fn fn);
int task_check(int condition, const char *expression, const char *file, int line);
void task_register_tests(void);
#define TASK_CHECK(expression) do { \
  if (!task_check(!!(expression), #expression, __FILE__, __LINE__)) return 1; \
} while (0)
#ifdef __cplusplus
}
#endif
#endif
