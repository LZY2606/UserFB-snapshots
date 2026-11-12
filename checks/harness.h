#pragma once
#include <vector>
#include <string>
#include <stdexcept>
struct Check { const char *group,*name; void (*fn)(); };
std::vector<Check>& checks();
struct AddCheck { AddCheck(const char*g,const char*n,void(*f)()) { checks().push_back({g,n,f}); } };
#define CHECK_CASE(G,N) static void N(); static AddCheck reg_##N(G,#N,N); static void N()
#define REQUIRE(X) do { if(!(X)) throw std::runtime_error(#X); } while(0)
