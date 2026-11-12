#pragma once
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace tcase {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& cases() { static std::vector<Case> v; return v; }
inline int& assertions() { static int n=0; return n; }
struct Add { Add(const char* n, void(*f)()) { cases().push_back({n,f}); } };
inline void check(bool ok, const char* expr, int line) {
 ++assertions(); if (!ok) throw std::runtime_error(std::string(expr)+" line "+std::to_string(line));
}
}
#define CASE(name) static void name(); static tcase::Add reg_##name(#name,name); static void name()
#define CHECK(expr) tcase::check(bool(expr),#expr,__LINE__)
