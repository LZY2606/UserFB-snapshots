#pragma once
#include <functional>
#include <stdexcept>
void contract_add(const char*, const char*, std::function<void()>);
void contract_expect(bool, const char*);
void register_contract();
