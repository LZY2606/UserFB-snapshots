#ifndef TASK_CHECK_H
#define TASK_CHECK_H
void task_check(int ok,const char *message,const char *file,int line);
#define TASK_CHECK(ok,message) task_check(!!(ok),message,__FILE__,__LINE__)
int task_group(const char *group);
#endif
