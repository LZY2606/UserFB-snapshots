#pragma once
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
struct Case { unsigned checks=0; void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);} };
struct Registry {struct Entry{std::string group,name;void(*fn)(Case&);};std::vector<Entry> entries;
 void add(const char*g,const char*n,void(*fn)(Case&)){entries.push_back({g,n,fn});}};
void register_task_tests(Registry&);
