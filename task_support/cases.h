#ifndef TASK_CASES_H
#define TASK_CASES_H
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {const char *group; const char *name; void (*run)(void);} TargetCase;
extern const TargetCase target_cases[];
extern const size_t target_count;
extern unsigned long target_checks;
#define TARGET_CHECK(c) do {++target_checks;if(!(c)){fprintf(stderr,"CHECK failed %s:%d: %s\n",__FILE__,__LINE__,#c);exit(2);}}while(0)
#endif
