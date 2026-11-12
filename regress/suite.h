#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
struct Check { private: unsigned n_=0; public: void expect(bool value){++n_;if(!value)throw std::runtime_error("assertion failed");}unsigned count()const{return n_;}};
struct Suite {std::vector<std::pair<std::string,std::function<void(Check&)>>> cases;void add(std::string name,std::function<void(Check&)> f){cases.emplace_back(name,f);}};
void register_tests(Suite&);
