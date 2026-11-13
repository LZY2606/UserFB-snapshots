#pragma once
#include <cstdio>
#include <cstdlib>
extern unsigned long task_checks;
inline void task_check(bool ok,const char*expr,int line){++task_checks;if(!ok){std::fprintf(stderr,"FAIL line=%d %s\n",line,expr);std::exit(2);}}
#define TASK_CHECK(x) task_check(bool(x),#x,__LINE__)
void task_run(const char* group);
