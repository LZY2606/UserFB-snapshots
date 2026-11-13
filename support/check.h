#pragma once
#include <cstdio>
#include <cstdlib>
extern int task_assertions;
#define TASK_CHECK(x) do{++task_assertions;if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(1);}}while(0)
void task_cases(const char* group);
