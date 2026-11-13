#include "check.h"
#include <string.h>
int main(void) {
 const char *groups[]={"hierarchy","predicates","provenance","files"}; unsigned seen[4]={0},total=0;
 if(!unit_case_count) return 2;
 for(unsigned i=0;i<unit_case_count;i++) {
  const unit_case *t=&unit_cases[i]; if(!t->name || !t->run) return 3;
  unit_context c={0}; t->run(&c); if(!c.checks) return 4;
  for(unsigned j=0;j<4;j++) if(!strcmp(t->group,groups[j])) seen[j]++;
  total+=c.checks; printf("PASS %s/%s checks=%u\n",t->group,t->name,c.checks);
 }
 for(unsigned j=0;j<4;j++) if(!seen[j]) return 5;
 printf("TARGET_COMPLETE cases=%u checks=%u\n",unit_case_count,total); return 0;
}
