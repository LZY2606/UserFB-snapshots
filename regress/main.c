#include "cases.h"
#include <stdio.h>
#include <string.h>
static const char *required[]={"NodeLists","StaticTypes","NothingComparisons","LegacyFunctions"};
static int seen[sizeof(required)/sizeof(required[0])],total,failed;
void task_case(const char *name, task_fn fn) {
 size_t i; for(i=0;i<sizeof(seen)/sizeof(seen[0]);i++) if(!strcmp(name,required[i])) break;
 if(i==sizeof(seen)/sizeof(seen[0]) || !fn || seen[i]) {failed=1;return;}
 seen[i]=1; total++; int rc=fn(); printf("%s %s\n",rc?"FAIL":"PASS",name); if(rc) failed=1;
}
int main(void) { task_register_tests(); for(size_t i=0;i<sizeof(seen)/sizeof(seen[0]);i++) if(!seen[i]) failed=1;
 printf("EXECUTED %d\n",total); return failed || total!=(int)(sizeof(seen)/sizeof(seen[0])); }
