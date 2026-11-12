#ifndef CHECK_CHECK_H
#define CHECK_CHECK_H
void unit_check(int ok,const char *expr,const char *file,int line);
#define UNIT_CHECK(x) unit_check(!!(x),#x,__FILE__,__LINE__)
#endif
