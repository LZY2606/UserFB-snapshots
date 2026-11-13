#ifndef TASK_CHECKS_H
#define TASK_CHECKS_H
#include <stdio.h>
#include <stdlib.h>
typedef struct {int assertions;} Check;
static void check(Check* c,int ok,const char* why){++c->assertions;if(!ok){fprintf(stderr,"%s\n",why);exit(1);}}
void test_projection(Check*);void test_material(Check*);void test_pipeline(Check*);
#endif
