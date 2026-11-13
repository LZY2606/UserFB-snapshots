/* test registration contract, CC0. */
#ifndef CHECK_CONTRACT_H
#define CHECK_CONTRACT_H
#include <stddef.h>
struct check_result { size_t checks; };
void check_check(struct check_result *, int, const char *, int);
#define Z_CHECK(r, expr) check_check((r),!!(expr),__FILE__,__LINE__)
void check_load(struct check_result *);
void check_play(struct check_result *);
void check_ramp(struct check_result *);
void check_formats(struct check_result *);
#endif
