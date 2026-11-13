#ifndef CHECK_CHECK_H
#define CHECK_CHECK_H
#ifdef __cplusplus
extern "C" {
#endif
void add_case(const char *group, const char *name, void (*fn)(void));
void check_value(int ok, const char *expr, int line);
void register_cases(void);
#ifdef __cplusplus
}
#endif
#define CHECK(x) check_value(!!(x), #x, __LINE__)
#endif
