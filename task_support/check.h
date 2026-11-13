#ifndef TASK_CHECK_H
#define TASK_CHECK_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned task_checks;
#define TASK_CHECK(x) do { task_checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(42); } } while(0)
void task_cases(void);
#endif
