#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
#ifdef __cplusplus
extern "C" {
#endif
void unit_add(const char*,void (*)(void));
void unit_expect(int,const char*,int);
void register_contract(void);
#ifdef __cplusplus
}
#endif
#define UNIT_CHECK(x) unit_expect(!!(x),#x,__LINE__)
#endif
