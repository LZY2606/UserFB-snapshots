#ifndef CHECK_TEST_SUPPORT_H
#define CHECK_TEST_SUPPORT_H
#include <cstdio>
#include <cstdlib>
namespace unit_test { inline unsigned& checks(){static unsigned n=0;return n;} inline void check(bool ok,const char* text,int line){++checks();if(!ok){std::fprintf(stderr,"assertion line %d: %s\n",line,text);std::exit(1);}} }
#define CHECK_CHECK(x) unit_test::check(!!(x),#x,__LINE__)
#endif
