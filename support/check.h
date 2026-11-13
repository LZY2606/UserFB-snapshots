#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
void unit_add(const char *name,void (*fn)(void));
void unit_expect(int ok,const char *expr,int line);
void register_contract(void);
#define UNIT_CHECK(x) unit_expect(!!(x),#x,__LINE__)
#endif
