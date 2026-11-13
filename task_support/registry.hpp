#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
struct Case {
  unsigned checks=0;
  void check(bool value,const std::string& what) {
    ++checks;
    if(!value) throw std::runtime_error(what);
  }
};
struct Registry {
  struct Entry { std::string group,name; std::function<void(Case&)> run; };
  std::vector<Entry> entries;
  void add(const std::string& group,const std::string& name,std::function<void(Case&)> fn) {
    entries.push_back({group,name,fn});
  }
};
void register_task_tests(Registry&);
