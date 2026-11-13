#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
struct Case {const char *g,*n; void (*fn)(void);};
static struct Case cases[256];static int nc,checks;
void add_case(const char *g,const char *n,void (*fn)(void)){if(nc==256||!g||!n||!fn)exit(2); cases[nc++]=(struct Case){g,n,fn};}
void check_value(int ok,const char *expr,int line){checks++;if(!ok){fprintf(stderr,"FAIL line %d: %s\n",line,expr);exit(1);}}
int main(void){
 const char *groups[]={"ellipse","world","persistence"};
 register_cases();
 for(unsigned g=0;g<sizeof(groups)/sizeof(*groups);g++){
  int count=0;
  for(int i=0;i<nc;i++)if(!strcmp(cases[i].g,groups[g])){
   int before=checks;cases[i].fn();if(checks==before){fprintf(stderr,"No assertions: %s\n",cases[i].n);return 2;}
   printf("PASS %s/%s\n",groups[g],cases[i].n);count++;
  }
  if(!count){fprintf(stderr,"Missing group: %s\n",groups[g]);return 2;}
 }
 printf("TARGET COMPLETE cases=%d checks=%d\n",nc,checks);return 0;
}
