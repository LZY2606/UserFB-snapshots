#ifndef ONIG_BUDGET_CHECK_H
#define ONIG_BUDGET_CHECK_H
void budget_check_impl(int ok, const char *expr, const char *file, int line);
#define BUDGET_CHECK(expr) budget_check_impl(!!(expr), #expr, __FILE__, __LINE__)
#endif
