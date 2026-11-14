#ifndef CHECK_HARNESS_H
#define CHECK_HARNESS_H
void check_assert(int ok, const char *file, int line);
void check_case(const char *group);
#define CHECK_CHECK(x) check_assert(!!(x), __FILE__, __LINE__)
#endif
