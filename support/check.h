#ifndef TASK_CHECK_H
#define TASK_CHECK_H
#include <stdio.h>
#include <stdlib.h>
extern int task_assertions;
#define TASK_CHECK(x) do{++task_assertions;if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
void task_cases(const char* group);
#endif
