#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
struct Case { unsigned assertions=0; void check(bool b,const char* msg) { ++assertions; if(!b) throw std::runtime_error(msg); } };
struct Registry { struct Entry { std::string group,name; std::function<void(Case&)> fn; }; std::vector<Entry> entries;
 void add(std::string group,std::string name,std::function<void(Case&)> fn) { entries.push_back({group,name,fn}); } };
void register_task_tests(Registry&);
