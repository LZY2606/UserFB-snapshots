#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
void unit_add(const char*,void (*)());
void unit_expect(bool,const char*,int);
void register_contract();
#define UNIT_CHECK(x) unit_expect(!!(x),#x,__LINE__)
#endif
