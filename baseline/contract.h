#ifndef CHECK_CONTRACT_H
#define CHECK_CONTRACT_H
void contract_add(const char *group, const char *name, void (*fn)(void));
void register_contract(void);
#endif
