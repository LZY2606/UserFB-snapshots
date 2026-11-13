#ifndef CHECK_CASE_HARNESS_H
#define CHECK_CASE_HARNESS_H
void unit_case_begin(void);
void unit_check(int ok, const char *expr, const char *file, int line);
#define CHECK_CASE() unit_case_begin()
#define UNIT_CHECK(expr) unit_check(!!(expr),#expr,__FILE__,__LINE__)
/* Tests define these four functions, with no main and no skip protocol. */
void test_description_mux(void);
void test_description_demux(void);
void test_description_continuation(void);
void test_description_invalid(void);
#endif
