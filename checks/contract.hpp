#pragma once
#include <functional>
void contract_add(const char *group,const char *name,std::function<void()> run);
void contract_expect(bool ok,const char *reason);
void register_contract();
