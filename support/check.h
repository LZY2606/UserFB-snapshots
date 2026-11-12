#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { const char *name; const char *group; int (*run)(void); } unit_case;
const unit_case *unit_cases(size_t *count);
extern size_t unit_assertions;
#ifdef __cplusplus
}
#endif
#define UNIT_CHECK(x) do { ++unit_assertions; if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);return 1;} } while(0)
#endif
