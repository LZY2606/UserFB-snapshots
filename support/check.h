#ifndef CHECK_CHECK_H
#define CHECK_CHECK_H
#include <stdio.h>
#include <stdlib.h>
extern unsigned long unit_checks;
extern unsigned long unit_failures;
#define UNIT_CHECK(expr) do { ++unit_checks; if (!(expr)) { ++unit_failures; fprintf(stderr,"UNIT_FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); } } while(0)
#endif
