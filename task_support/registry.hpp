#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
struct Registry {
 struct Case {std::string group,name; std::function<void()> fn;};
 std::vector<Case> cases; int assertions=0;
 void add(std::string group,std::string name,std::function<void()> fn){cases.push_back({group,name,fn});}
 void check(bool ok,const char* message){++assertions;if(!ok)throw std::runtime_error(message);}
};
void register_task_tests(Registry&);
