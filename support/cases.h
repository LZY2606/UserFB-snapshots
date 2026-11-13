#pragma once
#include <vector>
#include <utility>
#include <stdexcept>
using OfflineCase = std::pair<const char *, void (*)()>;
extern unsigned long task_assertions;
#define TASK_CHECK(x) do { ++task_assertions; if(!(x)) throw std::runtime_error(#x); } while(0)
void register_cases(std::vector<OfflineCase> &cases);
