#pragma once
#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <algorithm>
namespace checks { struct Case{std::string name;void(*fn)();};inline std::vector<Case>& cases(){static std::vector<Case> v;return v;} inline int assertions=0;struct Register{Register(const char* n,void(*f)()){cases().push_back({n,f});}};}
#define WELD_CHECK(x) do{++checks::assertions;if(!(x))throw std::runtime_error(#x);}while(0)
#define WELD_CASE(name) static void name();static checks::Register reg_##name(#name,name);static void name()
