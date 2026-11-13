#pragma once
#include <cstdio>
#include <cstdlib>
#include <vector>
struct TaskCase {const char* group;const char* name;void(*fn)();};
std::vector<TaskCase>& task_registry();
void task_check(bool ok,const char* expression,const char* file,int line);
struct TaskRegister {TaskRegister(const char* g,const char* n,void(*f)()){task_registry().push_back({g,n,f});}};
#define TASK_JOIN_(a,b) a##b
#define TASK_JOIN(a,b) TASK_JOIN_(a,b)
#define TASK_CASE(group,name) \
 static void TASK_JOIN(task_fn_,__LINE__)(); \
 static TaskRegister TASK_JOIN(task_reg_,__LINE__)(group,name,TASK_JOIN(task_fn_,__LINE__)); \
 static void TASK_JOIN(task_fn_,__LINE__)()
#define TASK_CHECK(expr) task_check(!!(expr),#expr,__FILE__,__LINE__)
