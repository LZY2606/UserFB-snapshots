#pragma once
#include <vector>
#include <string>
#include <stdexcept>
#include <iostream>
#include <algorithm>
namespace authorcheck { struct Case{std::string name;void(*fn)();};inline std::vector<Case>& cases(){static std::vector<Case> v;return v;} inline int assertions=0;struct Register{Register(const char* n,void(*f)()){cases().push_back({n,f});}};}
#define FBX_CHECK(x) do{++authorcheck::assertions;if(!(x))throw std::runtime_error(#x);}while(0)
#define FBX_CASE(name) static void name();static authorcheck::Register reg_##name(#name,name);static void name()
