#ifndef BPF_GRAPH_TASK_CHECK_H
#define BPF_GRAPH_TASK_CHECK_H
#include <cstddef>
bool task_check(bool value,const char* expression,const char* file,int line);
#define TASK_CHECK(x) do { if(!task_check(bool(x),#x,__FILE__,__LINE__)) return 1; } while(0)
struct task_case {const char* group; const char* name; int (*run)();};
extern const task_case task_cases[];
extern const std::size_t task_case_count;
#endif
