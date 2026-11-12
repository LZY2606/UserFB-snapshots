#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
struct ContentFailure : std::runtime_error {using std::runtime_error::runtime_error;};
struct Suite {
 struct Case {std::string name; std::function<void(Suite&)> fn;};
 std::vector<Case> cases; unsigned assertions=0;
 void add(std::string name,std::function<void(Suite&)> fn){cases.push_back({std::move(name),std::move(fn)});}
 void check(bool condition,const std::string& detail){++assertions;if(!condition)throw ContentFailure(detail);}
};
void register_tests(Suite&);
