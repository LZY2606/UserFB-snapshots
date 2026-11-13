#pragma once
#include <vector>
#include <string>
#include <stdexcept>
namespace task {
struct Case { unsigned checks=0; void check(bool b) {++checks;if(!b)throw std::runtime_error("assertion failed");} };
using Function=void(*)(Case &);
struct Entry {std::string group,name;Function fn;};
inline std::vector<Entry> &entries(){static std::vector<Entry> a;return a;}
struct Register {Register(const char*g,const char*n,Function f){entries().push_back({g,n,f});}};
}
#define TASK_CASE(group,name) static void name(task::Case&); static task::Register reg_##name(group,#name,name); static void name(task::Case &t)
