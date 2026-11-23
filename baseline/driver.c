#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct item {const char *group,*name; void (*fn)(void);};
static struct item items[256]; static unsigned count;
static const char *groups[]={"instructions","memory","barriers","branching"};
static void fail(void) {fputs("contract registration invalid\n",stderr);exit(9);}
void contract_add(const char *g,const char *n,void (*fn)(void)) {
 if(!g||!n||!*n||!fn||count==256) fail();
 unsigned k; for(k=0;k<sizeof(groups)/sizeof(groups[0]);k++) if(!strcmp(g,groups[k])) break;
 if(k==sizeof(groups)/sizeof(groups[0])) fail();
 for(unsigned i=0;i<count;i++) if(!strcmp(items[i].group,g)&&!strcmp(items[i].name,n)) fail();
 items[count++]=(struct item){g,n,fn};
}
int main(void) {
 register_contract(); if(!count) fail();
 for(unsigned k=0;k<sizeof(groups)/sizeof(groups[0]);k++) {
  unsigned ran=0;
  for(unsigned i=0;i<count;i++) if(!strcmp(items[i].group,groups[k])) {
   printf("CASE_BEGIN %s/%s\n",groups[k],items[i].name);fflush(stdout);
   items[i].fn(); ++ran; printf("CASE_PASS %s/%s\n",groups[k],items[i].name);
  }
  if(!ran) fail(); printf("GROUP_PASS %s cases=%u\n",groups[k],ran);
 }
 printf("CONTRACT_DONE cases=%u groups=%zu\n",count,sizeof(groups)/sizeof(groups[0])); return 0;
}
