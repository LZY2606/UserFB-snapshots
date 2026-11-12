#ifndef SPVM_CHECKPOINT_SUITE_H
#define SPVM_CHECKPOINT_SUITE_H
void unit_expect(int ok, const char *expr, const char *file, int line);
#define UNIT_CHECK(x) unit_expect(!!(x), #x, __FILE__, __LINE__)
/* target.c provides this function, NOT main().
 * Run the named group and return its actual UNIT_CHECK count. */
int unit_run_group(const char *name);
#endif
