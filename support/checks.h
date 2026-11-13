#ifndef TASK_CHECKS_H
#define TASK_CHECKS_H
#ifdef __cplusplus
extern "C" {
#endif
void task_check(int condition, const char* message);
void task_cases(const char* group);
#ifdef __cplusplus
}
#endif
#endif
