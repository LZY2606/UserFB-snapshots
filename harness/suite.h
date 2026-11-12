#ifndef CHECK_SUITE_H
#define CHECK_SUITE_H
#include <stddef.h>
typedef struct {unsigned checks,failures;} Check;
typedef void (*TestFn)(Check*);
typedef struct {const char*names[16];TestFn funcs[16];unsigned size;} Suite;
static inline void expect(Check*c,int v){c->checks++;if(!v)c->failures++;}
static inline void add_test(Suite*s,const char*n,TestFn f){if(s->size<16){s->names[s->size]=n;s->funcs[s->size++]=f;}}
void register_tests(Suite*);
#endif
