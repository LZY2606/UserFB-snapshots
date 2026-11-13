#ifndef TASK_REGISTRY_HPP
#define TASK_REGISTRY_HPP
#include <vector>
#include <string>
#include <cstdio>
#include <stdexcept>
struct Case {
    size_t checks;
    Case(): checks(0) {}
    void check(bool pass, const char* what) { ++checks; if(!pass) throw std::runtime_error(what); }
};
struct Registry {
    struct Entry { std::string group,name; int (*run)(Case&); };
    std::vector<Entry> entries;
    void add(const char* group,const char* name,int (*run)(Case&)) {
        if(!group || !name || !*name || !run) throw std::runtime_error("invalid registration");
        for(size_t i=0;i<entries.size();i++) if(entries[i].group==group && entries[i].name==name) throw std::runtime_error("duplicate case");
        Entry e={group,name,run}; entries.push_back(e);
    }
};
void register_task_tests(Registry&);
#endif
