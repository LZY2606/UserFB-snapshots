#ifndef OGG_REPAGE_CONTRACT_H
#define OGG_REPAGE_CONTRACT_H
typedef void (*contract_case)(void);
void contract_add(const char *group, const char *name, contract_case fn);
void contract_check(int ok, const char *expr, const char *file, int line);
#define contract_expect(v) contract_check(!!(v), #v, __FILE__, __LINE__)
void register_contract(void);
#endif
