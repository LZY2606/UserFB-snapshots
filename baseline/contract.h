#ifndef CONTRACT_H
#define CONTRACT_H
void contract_add(const char *group, const char *name, void (*fn)(void));
void contract_expect(int condition, const char *message);
void register_contract(void);
#endif
