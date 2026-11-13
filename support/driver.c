#include <stdio.h>
#include <stdlib.h>
#include "checks.h"
static unsigned assertions;
void task_check(int condition,const char *message) {
 ++assertions;
 if(!condition){fprintf(stderr,"ASSERT: %s\n",message);exit(2);}
}
int main(void) {
 const char *groups[]={"normal","boundary","compat"};
 for(unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);++i) {
  unsigned before=assertions;
  task_cases(groups[i]);
  if(assertions==before){fprintf(stderr,"EMPTY GROUP: %s\n",groups[i]);return 3;}
  printf("GROUP %s assertions=%u\n",groups[i],assertions-before);
 }
 printf("TARGET_COMPLETE assertions=%u\n",assertions);
 return 0;
}
