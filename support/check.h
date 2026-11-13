#pragma once
#include <stdio.h>
#include <stdlib.h>
typedef struct { const char *name; void (*run)(void); } TaskCase;
extern unsigned long task_assertions;
#define TASK_CHECK(x) do { ++task_assertions; if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
extern const TaskCase task_cases[];
extern const size_t task_case_count;
