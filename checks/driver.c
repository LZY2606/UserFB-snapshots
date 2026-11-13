/* Fixed group driver, CC0. */
#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
void check_check(struct check_result *r,int pass,const char *file,int line){
  r->checks++;if(!pass){fprintf(stderr,"CHECK_FAILED %s:%d\n",file,line);exit(1);}
}
int main(void){
  void (*groups[])(struct check_result *)={check_load,check_play,check_ramp,check_formats};
  const char *names[]={"load","play","ramp","formats"};
  for(size_t i=0;i<4;i++){
    struct check_result r={0};groups[i](&r);
    if(!r.checks){fprintf(stderr,"NO_CHECKS %s\n",names[i]);return 2;}
    printf("CHECK_GROUP %s checks=%zu DONE\n",names[i],r.checks);fflush(stdout);
  }
  puts("CHECK_TARGET groups=4 DONE");return 0;
}
