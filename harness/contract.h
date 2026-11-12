#ifndef CHECK_CONTRACT_H
#define CHECK_CONTRACT_H
void add_contract_case(const char *group,const char *name,void (*body)(void));
void register_contract_tests(void);
#endif
