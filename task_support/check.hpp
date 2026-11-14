#pragma once
#include <vector>
#include <string>
#include <stdexcept>
namespace contract {
struct Entry { std::string name; void (*fn)(); };
std::vector<Entry>& entries();
extern unsigned assertions;
struct Register { Register(const char* n, void(*f)()) { entries().push_back(Entry{n,f}); } };
inline void check(bool ok,const char* s,int line) { ++assertions; if(!ok) throw std::runtime_error(std::string(s)+":"+std::to_string(line)); }
}
#define CHECK(x) ::contract::check(bool(x),__FILE__,__LINE__)
#define CASE(n) static void case_##n(); static ::contract::Register reg_##n(#n,case_##n); static void case_##n()
