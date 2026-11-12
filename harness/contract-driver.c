#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct entry {const char *group,*name;void (*fn)(void);};
static struct entry cases[4096];static unsigned count;
void add_contract_case(const char *g,const char *n,void (*f)(void)){if(!g||!n||!f||count>=4096)exit(3);cases[count++]=(struct entry){g,n,f};}
int main(void){const char *groups[]={"existing-style","generated-rules","nested-owners","legacy-mode"};register_contract_tests();if(!count){fputs("NO_CASES\n",stderr);return 4;}
for(unsigned g=0;g<sizeof(groups)/sizeof(groups[0]);g++){unsigned ran=0;for(unsigned i=0;i<count;i++)if(!strcmp(cases[i].group,groups[g])){cases[i].fn();ran++;printf("PASS %s/%s\n",groups[g],cases[i].name);}if(!ran){fprintf(stderr,"EMPTY_GROUP %s\n",groups[g]);return 5;}printf("GROUP %s %u\n",groups[g],ran);}printf("CONTRACT_CASES=%u\n",count);return 0;}
