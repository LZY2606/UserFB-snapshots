#ifndef TASK_PRESSURE_REGISTRY_HPP
#define TASK_PRESSURE_REGISTRY_HPP
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>
namespace task {
struct Case {
    std::size_t checks=0;
    bool failed=false;
    void check(bool ok,const std::string& message) {
        ++checks;
        if(!ok) { failed=true; throw std::runtime_error(message); }
    }
};
using Function=void(*)(Case&);
struct Entry {const char* group;const char* name;Function fn;};
inline std::vector<Entry>& entries() {static std::vector<Entry> e;return e;}
struct Register {Register(const char* g,const char* n,Function f) {entries().push_back({g,n,f});}};
}
#define TASK_JOIN_INNER(a,b) a##b
#define TASK_JOIN(a,b) TASK_JOIN_INNER(a,b)
#define TASK_CASE(group,name) \
 static void name(task::Case&); \
 static task::Register TASK_JOIN(task_register_,name)(group,#name,name); \
 static void name(task::Case& c)
#endif
