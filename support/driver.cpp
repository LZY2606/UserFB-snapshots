#include <stdio.h>
#include <stdlib.h>
#include "checks.h"
static unsigned count;
void task_check(int ok,const char* message) {
 ++count;
 if(!ok){fprintf(stderr,"FAIL %s\n",message);exit(1);}
}
int main(int argc,char**argv){
 if(argc!=2)return 2;
 task_cases(argv[1]);
 if(count<5){fprintf(stderr,"too few assertions: %u\n",count);return 3;}
 printf("EXECUTED %s assertions=%u\n",argv[1],count);
 return 0;
}
