#pragma once
#include <stdexcept>
extern unsigned task_assertions;
#define TASK_CHECK(x) do { ++task_assertions; if(!(x)) throw std::runtime_error(#x); } while(0)
int contract_closure();
int contract_groups();
int contract_views();
