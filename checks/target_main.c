#include <stdio.h>
#include <stdlib.h>
#include "suite.h"
static int assertions;
void unit_expect(int ok,const char *expr,const char *file,int line) {
 if (!ok) {fprintf(stderr,"ASSERTION_FAILED %s:%d %s\n",file,line,expr);exit(1);}
 ++assertions;
}
int main(void) {
 const char *names[]={"relocate","continuation","reject"};
 int total=0;
 for (int i=0;i<3;++i) {
  assertions=0;
  int n=unit_run_group(names[i]);
  if (n<=0 || assertions<=0 || n!=assertions) {
   fprintf(stderr,"EMPTY_OR_FAILED %s claimed=%d actual=%d\n",names[i],n,assertions);return 1;
  }
  total+=assertions;
  printf("GROUP_OK %s %d\n",names[i],assertions);
 }
 printf("TARGET_OK %d\n",total);
 return 0;
}
